#include "game/light_test.h"

#include "core/alloc_count.h"
#include "core/log.h"
#include "game/level.h"
#include "sim/body.h"
#include "sim/combat.h"
#include "sim/weapons.h"
#include "sim/lighting.h"
#include "sim/props.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace dvk {

namespace {

bool expect(bool condition, const char* what) {
    log::info("  {:<58} {}", what, condition ? "ok" : "FAILED");
    return condition;
}

// The falloff written out again, straight from the text of shaders/wall.frag,
// so the check is against the shader rather than against the function being
// checked. If someone edits one of the two, this is what notices.
float shaderFalloff(float dist, float radius) {
    if (dist > radius) return 0.0f;   // the shader discards
    const float t = dist / radius;
    const float window = std::clamp(1.0f - t * t * t * t, 0.0f, 1.0f);
    return window * window / (1.0f + 12.0f * t * t);
}

// Ray against one triangle, Moller-Trumbore. Deliberately a different method
// from the segment walk it is used to check: no portals, no segment graph, no
// early out.
bool rayTriangle(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& a,
                 const glm::vec3& b, const glm::vec3& c, float& t) {
    constexpr float kEpsilon = 1e-7f;
    const glm::vec3 ab = b - a;
    const glm::vec3 ac = c - a;
    const glm::vec3 pv = glm::cross(dir, ac);
    const float det = glm::dot(ab, pv);
    if (std::abs(det) < kEpsilon) return false;
    const float inv = 1.0f / det;
    const glm::vec3 tv = origin - a;
    const float u = glm::dot(tv, pv) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    const glm::vec3 qv = glm::cross(tv, ab);
    const float v = glm::dot(dir, qv) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    t = glm::dot(ac, qv) * inv;
    return t > 1e-4f;
}

// Is anything solid between these two points, tested against EVERY wall
// triangle in the mine rather than by walking the segment graph? This is the
// independent method: if the walk ever misses an occluder, or invents one,
// the two verdicts differ and the test says so.
bool blockedByAnyWall(const Level& level, const glm::vec3& from, const glm::vec3& to,
                      float skin) {
    const glm::vec3 delta = to - from;
    const float distance = glm::length(delta);
    if (distance <= 1e-4f) return false;
    const glm::vec3 dir = delta / distance;
    for (const WallTriangle& wall : level.walls) {
        float t = 0.0f;
        if (!rayTriangle(from, dir, wall.p[0], wall.p[1], wall.p[2], t)) continue;
        if (t < distance - skin) return true;
    }
    return false;
}

} // namespace

int runLightTest(const char* levelPath) {
    Level level = loadLevel(levelPath);   // fatal on any structural problem
    bool ok = true;
    const int n = level.segmentCount();

    // --- the falloff matches the shader ---------------------------------
    log::info("falloff against shaders/wall.frag:");
    {
        float worst = 0.0f;
        bool monotonic = true;
        float previous = 2.0f;
        for (int i = 0; i <= 200; ++i) {
            const float radius = 30.0f;
            const float dist = radius * static_cast<float>(i) / 200.0f;
            const float mine = sim::lightFalloff(dist, radius);
            worst = std::max(worst, std::abs(mine - shaderFalloff(dist, radius)));
            if (mine > previous + 1e-6f) monotonic = false;
            previous = mine;
        }
        log::info("  worst difference over 200 samples: {:.9f}", worst);
        ok &= expect(worst < 1e-6f, "CPU falloff equals the shader's");
        ok &= expect(monotonic, "falloff never increases with distance");
        ok &= expect(sim::lightFalloff(30.0f, 30.0f) == 0.0f, "falloff is exactly zero at radius");
        ok &= expect(sim::lightFalloff(31.0f, 30.0f) == 0.0f, "falloff is zero beyond radius");
        ok &= expect(sim::lightFalloff(0.0f, 30.0f) > 0.9f, "falloff is near one at the source");
    }

    // --- the spot cone ---------------------------------------------------
    log::info("spot cone:");
    {
        Light spot;
        spot.type = LightType::Spot;
        spot.position = glm::vec3(0.0f);
        spot.direction = glm::vec3(0.0f, 0.0f, -1.0f);
        spot.coneDegrees = 60.0f;
        spot.radius = 40.0f;
        spot.intensity = 1.0f;

        const float onAxis = sim::lightContribution(spot, glm::vec3(0, 0, -10));
        const float justInside = sim::lightContribution(
            spot, glm::vec3(std::tan(glm::radians(20.0f)) * 10.0f, 0, -10));
        const float outside = sim::lightContribution(
            spot, glm::vec3(std::tan(glm::radians(40.0f)) * 10.0f, 0, -10));
        const float behind = sim::lightContribution(spot, glm::vec3(0, 0, 10));
        log::info("  on axis {:.3f}, 20 deg off {:.3f}, 40 deg off {:.3f}, behind {:.3f}", onAxis,
                  justInside, outside, behind);
        ok &= expect(onAxis > 0.0f, "a spot lights along its own axis");
        ok &= expect(justInside > 0.0f && justInside <= onAxis,
                     "inside the cone is lit, and never brighter than the axis");
        ok &= expect(outside == 0.0f, "outside the cone is dark");
        ok &= expect(behind == 0.0f, "behind a spot is dark");

        Light point = spot;
        point.type = LightType::Point;
        ok &= expect(sim::lightContribution(point, glm::vec3(0, 0, 10)) > 0.0f,
                     "the same light as a point lights behind itself");
    }

    // --- occlusion actually occludes -------------------------------------
    //
    // The cheap failure mode for a query like this is that the shadow ray never
    // hits anything and the whole thing quietly degrades to the transparent
    // brightness --world-test already reports. So find a place where the two
    // answers differ, and fail if none exists.
    log::info("occlusion, over a probe set inside every segment:");
    int occluded = 0;
    int probes = 0;
    float biggestDrop = 0.0f;
    int dropSegment = -1;
    for (int si = 0; si < n; ++si) {
        const Segment& segment = level.segments[static_cast<size_t>(si)];

        // Centres alone find nothing: a light only reaches a segment through a
        // portal, and a centre can almost always see straight back out of that
        // portal to the light. The corners are where the mine gets between the
        // two, so probe towards them as well as at the middle.
        std::array<glm::vec3, 9> points{};
        points[0] = segment.center;
        for (int v = 0; v < 8; ++v) {
            const glm::vec3 corner = level.vertices[static_cast<size_t>(segment.verts[v])];
            points[static_cast<size_t>(v) + 1] = segment.center + (corner - segment.center) * 0.7f;
        }

        for (const glm::vec3& point : points) {
            sim::IlluminationQuery query;
            query.point = point;
            query.segment = si;

            query.shadows = false;
            const float open = sim::illuminationAt(query, level, level.lights).value;
            query.shadows = true;
            const float shadowed = sim::illuminationAt(query, level, level.lights).value;
            ++probes;

            if (shadowed < open - 1e-4f) {
                ++occluded;
                if (open - shadowed > biggestDrop) {
                    biggestDrop = open - shadowed;
                    dropSegment = si;
                }
            }
            if (shadowed > open + 1e-4f) {
                log::error("segment {}: shadowed {:.3f} exceeds unshadowed {:.3f}", si, shadowed,
                           open);
                ok = false;
            }
        }
    }
    log::info("  {} of {} probes lose light to a wall; biggest drop {:.3f} in segment {}", occluded,
              probes, biggestDrop, dropSegment);
    ok &= expect(occluded > 0, "shadow rays block something somewhere");

    // --- the segment walk against brute force -----------------------------
    //
    // Seven probes in three hundred is a small enough number to want proof it
    // is not seven out of a hundred missed. So ask the same question the other
    // way: test the ray against every wall triangle in the mine, with no
    // segment graph involved at all, and require the two to return the same
    // verdict every time.
    log::info("shadow rays against brute force over all {} wall triangles:", level.walls.size());
    {
        int compared = 0;
        int blocked = 0;
        int disagreements = 0;
        // One light at a time, so a disagreement names the light rather than a
        // sum. Hoisted and reused rather than built per pair.
        std::vector<Light> one(1);
        for (int si = 0; si < n; ++si) {
            const Segment& segment = level.segments[static_cast<size_t>(si)];
            std::array<glm::vec3, 9> points{};
            points[0] = segment.center;
            for (int v = 0; v < 8; ++v) {
                const glm::vec3 corner = level.vertices[static_cast<size_t>(segment.verts[v])];
                points[static_cast<size_t>(v) + 1] =
                    segment.center + (corner - segment.center) * 0.7f;
            }

            for (const glm::vec3& point : points) {
                for (const Light& light : level.lights) {
                    if (std::find(light.segments.begin(), light.segments.end(), si) ==
                        light.segments.end()) {
                        continue;
                    }
                    if (sim::lightContribution(light, point) <= 0.0f) continue;

                    sim::IlluminationQuery query;
                    query.point = point;
                    query.segment = si;
                    one[0] = light;
                    const bool walkLit = sim::illuminationAt(query, level, one).value > 0.0f;
                    const bool bruteLit = !blockedByAnyWall(level, point, light.position, 0.05f);

                    ++compared;
                    if (!bruteLit) ++blocked;
                    if (walkLit != bruteLit) {
                        ++disagreements;
                        if (disagreements <= 4) {
                            log::error(
                                "segment {} at ({:.2f} {:.2f} {:.2f}): the walk says {}, brute "
                                "force says {}",
                                si, point.x, point.y, point.z, walkLit ? "lit" : "shadowed",
                                bruteLit ? "lit" : "shadowed");
                        }
                    }
                }
            }
        }
        log::info("  {} point-light pairs compared, {} of them blocked, {} disagreements", compared,
                  blocked, disagreements);
        ok &= expect(compared > 0, "there were pairs to compare");
        ok &= expect(disagreements == 0, "the segment walk agrees with brute force every time");
    }

    // --- the level has somewhere dark and somewhere lit -------------------
    log::info("darkness at segment centres (all lights, shadowed):");
    int dark = 0;
    int lit = 0;
    for (int si = 0; si < n; ++si) {
        sim::IlluminationQuery query;
        query.point = level.segments[static_cast<size_t>(si)].center;
        query.segment = si;
        const sim::Illumination result = sim::illuminationAt(query, level, level.lights);
        if (result.dark()) ++dark;
        else ++lit;
        log::info("  segment {:2}: {:.3f}  {:<4}  {} of {} lights reach, {} visible", si,
                  result.value, result.dark() ? "dark" : "lit", result.lightsReaching,
                  level.lights.size(), result.lightsVisible);
    }
    log::info("  {} dark, {} lit, threshold {:.2f}", dark, lit, sim::kDarkThreshold);
    ok &= expect(dark > 0, "the level has somewhere dark to stand");
    ok &= expect(lit > 0, "the level has somewhere lit to stand");

    // --- every light the simulation owns reaches the detection meter -------
    //
    // playerVisibility is the number the robots act on AND the number the HUD's
    // detection meter shows. A light that reaches the screen without reaching
    // this function is a room the player can see is lit while the game insists
    // they are hidden in it - which is what happened when the floating and
    // swinging lights were added and nothing told this query about them.
    //
    // One case per kind, and the count is checked against kSimLightSources, so
    // a sixth kind breaks this test rather than shipping half-wired.
    {
        const sim::WeaponConfig weapons = sim::WeaponConfig::defaults();

        // Somewhere the level's own lights do not reach, so any reading is the
        // one this case put there.
        int darkSegment = -1;
        for (int si = 0; si < n && darkSegment < 0; ++si) {
            sim::IlluminationQuery probe;
            probe.point = level.segments[static_cast<size_t>(si)].center;
            probe.segment = si;
            if (sim::illuminationAt(probe, level, level.lights).dark()) darkSegment = si;
        }
        ok &= expect(darkSegment >= 0, "the level has a dark segment to test in");

        if (darkSegment >= 0) {
            const glm::vec3 at = level.segments[static_cast<size_t>(darkSegment)].center;
            const auto seen = [&](const sim::CombatState& combat, const sim::PropState& props) {
                return sim::playerVisibility(level, combat, weapons, at, darkSegment, nullptr,
                                             &props);
            };

            sim::CombatState empty;
            sim::PropState noProps;
            const sim::PlayerVisibility baseline = seen(empty, noProps);
            ok &= expect(!baseline.visible, "the player is hidden there with nothing lit");

            int wired = 0;

            {   // 1. a bolt in flight
                sim::CombatState combat;
                sim::Projectile& p = combat.projectiles.items[0];
                p.alive = true;
                p.position = at + glm::vec3(0.0f, 2.0f, 0.0f);
                p.lightRadius = 18.0f;
                p.lightIntensity = 3.0f;
                if (expect(seen(combat, noProps).visible, "a bolt in flight gives the player away")) {
                    ++wired;
                }
            }

            {   // 2. a burning flare
                sim::CombatState combat;
                sim::Flare& f = combat.flares.items[0];
                f.alive = true;
                f.position = at + glm::vec3(0.0f, 2.0f, 0.0f);
                f.lifetime = 30.0f;
                if (expect(seen(combat, noProps).visible, "so does a burning flare")) ++wired;
            }

            {   // 3. an explosion or muzzle flash
                sim::CombatState combat;
                sim::Effect& e = combat.effects.items[0];
                e.alive = true;
                e.position = at + glm::vec3(0.0f, 2.0f, 0.0f);
                e.lifetime = 0.4f;
                e.lightRadius = 20.0f;
                e.lightIntensity = 4.0f;
                if (expect(seen(combat, noProps).visible, "so does an explosion flash")) ++wired;
            }

            {   // 4. a floating light
                sim::PropState props;
                props.floatingCount = 1;
                sim::FloatingLight& ball = props.floating[0];
                ball.alive = true;
                ball.body.position = at + glm::vec3(0.0f, 3.0f, 0.0f);
                ball.body.segment = darkSegment;
                if (expect(seen(empty, props).visible, "so does a floating light")) ++wired;
            }

            {   // 5. a swinging lamp, which is a SPOT - the cone is the point
                sim::PropState props;
                props.swingingCount = 1;
                sim::SwingingLight& lamp = props.swinging[0];
                lamp.alive = true;
                lamp.position = at + glm::vec3(0.0f, 6.0f, 0.0f);
                lamp.segment = darkSegment;
                lamp.direction = glm::vec3(0.0f, -1.0f, 0.0f);
                if (expect(seen(empty, props).visible, "so does a swinging lamp pointed at you")) {
                    ++wired;
                }

                // And the case that makes the cone worth carrying: the same
                // lamp, the same distance, aimed the other way. Treated as a
                // point light this would read as lit, which is the opposite of
                // what the player can see.
                lamp.direction = glm::vec3(0.0f, 1.0f, 0.0f);
                ok &= expect(!seen(empty, props).visible,
                             "but a lamp aimed away from you leaves you in the dark");
            }

            log::info("  {} of {} simulation-owned light kinds reach playerVisibility", wired,
                      sim::kSimLightSources);
            ok &= expect(wired == sim::kSimLightSources,
                         "every light the simulation owns reaches the detection meter");
        }
    }

    // --- the body sphere --------------------------------------------------
    //
    // A point answer flips the instant a shadow edge crosses it. Sampling the
    // body is the whole reason litFraction exists, so check that it is
    // consistent with the value and report whether the level ever produces a
    // partly lit body at all.
    log::info("body sphere, radius {:.2f}:", sim::Body{}.radius);
    {
        int partial = 0;
        bool consistent = true;
        int samples = 0;
        for (int si = 0; si < n; ++si) {
            sim::IlluminationQuery query;
            query.point = level.segments[static_cast<size_t>(si)].center;
            query.segment = si;
            query.bodyRadius = sim::Body{}.radius;
            const sim::Illumination result = sim::illuminationAt(query, level, level.lights);
            samples = result.samples;
            const float fraction = result.litFraction();
            if (fraction > 0.0f && fraction < 1.0f) ++partial;
            if ((result.value > 0.0f) != (fraction > 0.0f)) {
                log::error("segment {}: value {:.3f} but lit fraction {:.2f}", si, result.value,
                           fraction);
                consistent = false;
            }
        }
        log::info("  {} samples per query; {} of {} segment centres are partly lit", samples,
                  partial, n);
        ok &= expect(samples == sim::kIlluminationSamples, "a body query takes every sample");
        ok &= expect(consistent, "lit fraction and value agree about darkness");
    }

    // --- what the robots ask ----------------------------------------------
    //
    // playerVisibility is the rule the AI acts on, so it is checked as a rule
    // and not only as arithmetic: the headlight has to beat darkness, a bolt
    // going off next to the player has to beat darkness, and neither may be
    // needed somewhere already lit.
    log::info("player visibility:");
    {
        sim::CombatState combat;
        const sim::WeaponConfig weapons;

        int darkSegment = -1;
        int litSegment = -1;
        for (int si = 0; si < n && (darkSegment < 0 || litSegment < 0); ++si) {
            const glm::vec3 p = level.segments[static_cast<size_t>(si)].center;
            const sim::PlayerVisibility v = sim::playerVisibility(level, combat, weapons, p, si);
            if (!v.visible && darkSegment < 0) darkSegment = si;
            if (v.visible && litSegment < 0) litSegment = si;
        }
        ok &= expect(darkSegment >= 0 && litSegment >= 0,
                     "the level has both a dark and a lit place to stand");

        if (darkSegment >= 0 && litSegment >= 0) {
            const glm::vec3 dark = level.segments[static_cast<size_t>(darkSegment)].center;
            const glm::vec3 lit = level.segments[static_cast<size_t>(litSegment)].center;

            const sim::PlayerVisibility hidden =
                sim::playerVisibility(level, combat, weapons, dark, darkSegment);
            ok &= expect(!hidden.visible, "a player in an unlit segment is not visible");
            ok &= expect(!hidden.headlight, "and is not carrying a light");

            combat.player.headlightOn = true;
            const sim::PlayerVisibility lamp =
                sim::playerVisibility(level, combat, weapons, dark, darkSegment);
            ok &= expect(lamp.visible && lamp.headlight,
                         "the headlight makes them visible in the same spot");
            combat.player.headlightOn = false;

            // A bolt in flight, right beside them. The headlight is off, so the
            // only thing that can light them is the loose light.
            const int slot = combat.projectiles.spawn();
            sim::Projectile& bolt = combat.projectiles.items[static_cast<size_t>(slot)];
            bolt.position = dark + glm::vec3(1.0f, 0.0f, 0.0f);
            bolt.segment = darkSegment;
            bolt.lightRadius = 18.0f;
            bolt.lightIntensity = 2.4f;
            const sim::PlayerVisibility byBolt =
                sim::playerVisibility(level, combat, weapons, dark, darkSegment);
            log::info("  dark {:.3f}, with a bolt alongside {:.3f}", hidden.illumination,
                      byBolt.illumination);
            ok &= expect(byBolt.illumination > hidden.illumination,
                         "a bolt alongside the player adds light");
            ok &= expect(byBolt.visible, "and gives their position away");
            combat.projectiles.items[static_cast<size_t>(slot)].alive = false;

            const sim::PlayerVisibility open =
                sim::playerVisibility(level, combat, weapons, lit, litSegment);
            ok &= expect(open.visible && !open.headlight,
                         "a player in a lit segment is visible with no light of their own");
        }
    }

    // --- the query allocates nothing --------------------------------------
    //
    // CLAUDE.md forbids per-frame heap allocation in the sim step, and this is
    // meant to be callable from there. Measured rather than argued.
    log::info("allocation:");
    {
        sim::IlluminationQuery query;
        query.point = level.segments[0].center;
        query.segment = 0;
        query.bodyRadius = sim::Body{}.radius;

        sim::Illumination warm = sim::illuminationAt(query, level, level.lights);
        (void)warm;

        alloc::Scope scope;
        float total = 0.0f;
        for (int i = 0; i < 1000; ++i) {
            for (int si = 0; si < n; ++si) {
                query.point = level.segments[static_cast<size_t>(si)].center;
                query.segment = si;
                total += sim::illuminationAt(query, level, level.lights).value;
            }
        }
        const uint64_t allocations = scope.count();
        log::info("  {} queries over {} segments: {} allocation(s), total {:.1f}", 1000 * n, n,
                  allocations, total);
        ok &= expect(allocations == 0, "1000 sweeps of the level allocate nothing");

        // playerVisibility runs every simulation step, and builds a stack Light
        // for each loose light it gathers. That costs nothing only as long as
        // the light's segment vector is left alone, which is easy to break by
        // accident, so it is measured rather than assumed.
        sim::CombatState combat;
        const sim::WeaponConfig weapons;
        const int slot = combat.projectiles.spawn();
        sim::Projectile& bolt = combat.projectiles.items[static_cast<size_t>(slot)];
        bolt.position = level.segments[0].center;
        bolt.segment = 0;
        bolt.lightRadius = 18.0f;
        bolt.lightIntensity = 2.4f;

        sim::PlayerVisibility warmV =
            sim::playerVisibility(level, combat, weapons, level.segments[0].center, 0);
        (void)warmV;

        alloc::Scope visScope;
        int seen = 0;
        for (int i = 0; i < 1000; ++i) {
            for (int si = 0; si < n; ++si) {
                const glm::vec3 p = level.segments[static_cast<size_t>(si)].center;
                seen += sim::playerVisibility(level, combat, weapons, p, si).visible ? 1 : 0;
            }
        }
        const uint64_t visAllocations = visScope.count();
        log::info("  {} visibility queries: {} allocation(s), {} visible", 1000 * n,
                  visAllocations, seen);
        ok &= expect(visAllocations == 0, "1000 sweeps of playerVisibility allocate nothing");
    }

    log::info("light test: {}", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace dvk
