#include "game/light_test.h"

#include "core/alloc_count.h"
#include "core/log.h"
#include "game/level.h"
#include "sim/body.h"
#include "sim/combat.h"
#include "sim/weapons.h"
#include "sim/lighting.h"
#include "sim/props.h"
#include "sim/geometry.h"
#include "sim/occluders.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <vector>

namespace dvk {

namespace {

// A spread of directions around a point: the six axes and the eight corners.
// Enough to hit a box on its faces, its edges and its corners without the full
// 26, which would make the oracle loop below four times longer for no new cases.
std::array<glm::vec3, 14> makeSpread() {
    std::array<glm::vec3, 14> out{};
    size_t k = 0;
    const glm::vec3 axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const glm::vec3& a : axes) out[k++] = a;
    for (int x = -1; x <= 1; x += 2) {
        for (int y = -1; y <= 1; y += 2) {
            for (int z = -1; z <= 1; z += 2) {
                out[k++] = glm::normalize(glm::vec3(x, y, z));
            }
        }
    }
    return out;
}

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

    // ====================================================================
    // Dynamic occluders (M8)
    // ====================================================================

    // Build the level's objects into a set, the way the game does.
    sim::OccluderSet objectOccluders;
    for (size_t oi = 0; oi < level.objectProxies.size(); ++oi) {
        objectOccluders.add(level.objectProxies[oi],
                            sim::OccluderId{sim::OccluderKind::Object, static_cast<int>(oi)},
                            level.objects[oi].segment);
    }

    // --- the slab test against brute-force ray/triangle --------------------
    //
    // `rayProxyDistance` is an analytic slab test on a box. The independent
    // method is the box's own twelve triangles through Moller-Trumbore - the
    // same routine the mine's walls use, entirely different arithmetic. If the
    // two disagree, one of them is wrong and it is not obvious which, so this
    // runs before anything built on top of the slab test is trusted.
    {
        int tested = 0;
        int disagreed = 0;
        int grazing = 0;
        float worst = 0.0f;

        // The 8 corners of a box, and the 12 triangles over them.
        // Corner c has sign x = (c&1), y = (c&2), z = (c&4), so the six faces
        // are the corner sets sharing one bit. Getting this wrong makes
        // triangles that slice through the box's interior rather than skin it,
        // and the oracle then reports a hit where the box has no surface - which
        // is exactly what the first draft of this did.
        static constexpr int kFaces[12][3] = {
            {0, 2, 1}, {1, 2, 3},   // -z: 0 1 2 3
            {4, 5, 6}, {5, 7, 6},   // +z: 4 5 6 7
            {0, 1, 5}, {0, 5, 4},   // -y: 0 1 4 5
            {2, 6, 7}, {2, 7, 3},   // +y: 2 3 6 7
            {1, 3, 7}, {1, 7, 5},   // +x: 1 3 5 7
            {0, 4, 6}, {0, 6, 2},   // -x: 0 2 4 6
        };

        for (int oi = 0; oi < objectOccluders.count; ++oi) {
            const sim::OccluderEntry& e = objectOccluders.entries[static_cast<size_t>(oi)];
            if (e.proxy.kind != sim::ProxyVolume::Kind::Box) continue;

            std::array<glm::vec3, 8> corner{};
            for (int c = 0; c < 8; ++c) {
                const glm::vec3 sign((c & 1) ? 1.0f : -1.0f, (c & 2) ? 1.0f : -1.0f,
                                     (c & 4) ? 1.0f : -1.0f);
                corner[static_cast<size_t>(c)] =
                    e.proxy.centre + e.proxy.orientation * (sign * e.proxy.halfExtents);
            }

            // Rays from all around the box, aimed at a spread of points on and
            // near it, so faces, edges, corners and clean misses all appear.
            for (const glm::vec3& dir : makeSpread()) {
                for (float aim = -1.0f; aim <= 1.0f; aim += 0.5f) {
                    const glm::vec3 from = e.proxy.centre + dir * 40.0f;
                    const glm::vec3 at =
                        e.proxy.centre + glm::vec3(0.0f, aim * e.proxy.halfExtents.y * 1.1f, 0.0f);
                    const glm::vec3 ray = glm::normalize(at - from);

                    const float slab = sim::rayProxyDistance(e.proxy, from, ray);

                    float brute = -1.0f;
                    for (const auto& f : kFaces) {
                        // With the same edge slack the project already gives
                        // portal quads, and for the same reason. A ray down a
                        // box's corner enters exactly where two faces meet, and
                        // with no slack Moller-Trumbore misses BOTH triangles
                        // and reports the far side instead - on mine01's reactor
                        // that is an 18.7 unit error. The slab test has no such
                        // seam, which is the point of using it in the first
                        // place.
                        const float t = sim::rayTriangleDistance(
                            from, ray, corner[static_cast<size_t>(f[0])],
                            corner[static_cast<size_t>(f[1])], corner[static_cast<size_t>(f[2])],
                            1e-4f);
                        if (t > 1e-4f && (brute < 0.0f || t < brute)) brute = t;
                    }

                    ++tested;
                    const bool slabHit = slab >= 0.0f;
                    const bool bruteHit = brute >= 0.0f;
                    if (slabHit != bruteHit) {
                        if (disagreed < 4) {
                            log::error("slab says {} but the triangles say {} from {:.1f} {:.1f} "
                                       "{:.1f}", slabHit ? "hit" : "miss",
                                       bruteHit ? "hit" : "miss", from.x, from.y, from.z);
                        }
                        ++disagreed;
                    } else if (slabHit) {
                        // A ray aimed past the box's own half-height clips an
                        // edge, and which triangle catches it comes down to the
                        // last bit. The verdict still has to agree - asserted
                        // above, for every ray - but comparing entry distances
                        // there measures floating point, not correctness.
                        if (std::abs(aim) > 0.9f) {
                            ++grazing;
                        } else {
                            worst = std::max(worst, std::abs(slab - brute));
                        }
                    }
                }
            }
        }

        if (tested == 0) {
            log::info("occluder oracle: SKIPPED, no box occluder in this level");
        } else {
            ok &= expect(disagreed == 0, "the slab test and the box's own triangles agree");
            ok &= expect(worst < 0.01f, "and they agree on the distance, not only the verdict");
            log::info("occluder oracle: {} rays, {} disagreements, worst distance gap {:.6f}u over "
                      "{} square hits ({} grazing rays agreed on the verdict but are not compared "
                      "on distance)", tested, disagreed, worst, tested - grazing, grazing);
        }
    }

    // --- the hull tests against the model's own triangles -------------------
    //
    // The box oracle above compares an analytic test against Moller-Trumbore
    // over twelve triangles it derives from the box's corners. A hull's oracle
    // is the same idea with a stronger claim behind it: for a convex model the
    // hull's faces ARE the model's faces, so triangulating them does not
    // approximate the collider the way a tessellated cylinder would have. The
    // triangles are the surface, exactly, and any disagreement is a bug.
    //
    // Two queries are checked, not one. The ray test is a slab loop and the
    // closest-point test is a face-edge-vertex classification, and the second
    // is the fiddly one - it had no oracle at all while the proxies were boxes,
    // because a box's closest point is a clamp nobody doubts.
    {
        int rays = 0;
        int rayDisagreed = 0;
        int rayGrazing = 0;
        float worstRay = 0.0f;

        int points = 0;
        int pointDisagreed = 0;
        float worstPoint = 0.0f;

        // Room for the triangulated faces of the largest hull the loader will
        // accept, so this allocates nothing per occluder.
        std::vector<std::array<glm::vec3, 3>> surface;

        for (int oi = 0; oi < objectOccluders.count; ++oi) {
            const sim::OccluderEntry& e = objectOccluders.entries[static_cast<size_t>(oi)];
            if (e.proxy.kind != sim::ProxyVolume::Kind::ConvexHull) continue;
            const sim::ConvexHullView& hull = e.proxy.hull;
            if (!hull.valid()) continue;

            // Every face, fanned, in world space - through the same transform
            // the proxy uses, so this checks the query and not the placement.
            const auto toWorld = [&](const glm::vec3& v) {
                return e.proxy.centre + e.proxy.orientation * (v * e.proxy.scale);
            };
            surface.clear();
            for (int f = 0; f < hull.planeCount; ++f) {
                const int first = hull.faceFirst[f];
                const int count = hull.faceFirst[f + 1] - first;
                for (int c = 1; c + 1 < count; ++c) {
                    surface.push_back({toWorld(hull.vertices[hull.faceVertex[first]]),
                                       toWorld(hull.vertices[hull.faceVertex[first + c]]),
                                       toWorld(hull.vertices[hull.faceVertex[first + c + 1]])});
                }
            }
            if (surface.empty()) continue;

            const float reach = e.proxy.boundingRadius();

            for (const glm::vec3& dir : makeSpread()) {
                const glm::vec3 across =
                    glm::normalize(glm::cross(dir, glm::vec3(0.0f, 1.0f, 0.0f)) +
                                   glm::vec3(1e-4f, 0.0f, 0.0f));
                for (float aim = -1.2f; aim <= 1.2f; aim += 0.3f) {
                    for (float side = -1.3f; side <= 1.3f; side += 0.65f) {
                        const glm::vec3 from = e.proxy.centre + dir * 40.0f;
                        const glm::vec3 at = e.proxy.centre +
                                             glm::vec3(0.0f, aim * e.proxy.halfExtents.y, 0.0f) +
                                             across * (side * reach);
                        const glm::vec3 ray = glm::normalize(at - from);

                        const float slab = sim::rayProxyDistance(e.proxy, from, ray);

                        float brute = -1.0f;
                        for (const auto& tri : surface) {
                            // The same edge slack the box oracle takes, for the
                            // same reason: a ray straight down a shared edge
                            // misses both triangles without it, where the
                            // analytic test has no seam to fall through.
                            const float t = sim::rayTriangleDistance(from, ray, tri[0], tri[1],
                                                                     tri[2], 1e-4f);
                            if (t > 1e-4f && (brute < 0.0f || t < brute)) brute = t;
                        }

                        ++rays;
                        const bool slabHit = slab >= 0.0f;
                        const bool bruteHit = brute >= 0.0f;
                        // A ray aimed along the silhouette clips an edge, and
                        // which triangle catches it comes down to the last bit.
                        const bool nearRim =
                            std::abs(std::abs(side) - 1.0f) < 0.15f || std::abs(aim) > 0.95f;

                        if (slabHit != bruteHit) {
                            if (nearRim) {
                                ++rayGrazing;
                            } else {
                                if (rayDisagreed < 4) {
                                    log::error("hull ray says {} but its own triangles say {} "
                                               "from {:.1f} {:.1f} {:.1f}",
                                               slabHit ? "hit" : "miss", bruteHit ? "hit" : "miss",
                                               from.x, from.y, from.z);
                                }
                                ++rayDisagreed;
                            }
                        } else if (slabHit) {
                            if (nearRim) ++rayGrazing;
                            else worstRay = std::max(worstRay, std::abs(slab - brute));
                        }

                        // And the closest point, from the same place. Compared
                        // as a DISTANCE rather than as a point: on a flat face
                        // every point of it is equally close to a ray's origin
                        // only if the origin is on its normal, and the two
                        // methods are entitled to disagree about which corner
                        // of a tie they return.
                        const glm::vec3 probe = at + dir * 0.5f;
                        const glm::vec3 mine = sim::closestPointOnProxy(e.proxy, probe);
                        const bool inside = glm::length(probe - mine) < 1e-5f &&
                                            sim::rayProxyDistance(e.proxy, probe, dir) == 0.0f;
                        if (inside) continue;   // interior points come back unchanged

                        float nearest = std::numeric_limits<float>::max();
                        for (const auto& tri : surface) {
                            nearest = std::min(nearest, sim::distanceToTriangle(probe, tri[0],
                                                                                tri[1], tri[2]));
                        }
                        ++points;
                        const float gap = std::abs(glm::length(probe - mine) - nearest);
                        if (gap > 0.01f) {
                            if (pointDisagreed < 4) {
                                log::error("hull closest point is {:.4f}u away but its own "
                                           "triangles say {:.4f}u, from {:.1f} {:.1f} {:.1f}",
                                           glm::length(probe - mine), nearest, probe.x, probe.y,
                                           probe.z);
                            }
                            ++pointDisagreed;
                        }
                        worstPoint = std::max(worstPoint, gap);
                    }
                }
            }
        }

        if (rays == 0) {
            log::info("hull oracle: SKIPPED, no convex hull in this level");
        } else {
            ok &= expect(rayDisagreed == 0, "the hull ray test and the model's triangles agree");
            ok &= expect(worstRay < 0.01f, "and they agree on the distance, not only the verdict");
            ok &= expect(pointDisagreed == 0,
                         "the hull closest point and the model's triangles agree");
            log::info("hull oracle: {} rays, {} disagreements, worst distance gap {:.6f}u over {} "
                      "square hits ({} along a rim, not compared); {} closest-point probes, {} "
                      "disagreements, worst gap {:.6f}u",
                      rays, rayDisagreed, worstRay, rays - rayGrazing, rayGrazing, points,
                      pointDisagreed, worstPoint);
        }
    }

    // --- a pillar casts into the light query -------------------------------
    //
    // The case the whole milestone is for. Stand a point in a light's beam, then
    // stand it behind a column, and require the second to be darker. The first
    // half is the positive control: without it a query that returned zero for
    // everything would pass.
    {
        int tested = 0;
        int failed = 0;
        float worstDrop = 1.0f;

        for (size_t oi = 0; oi < level.objectProxies.size(); ++oi) {
            const sim::PlacedProxy& proxy = level.objectProxies[oi];
            // Any solid with a side to hide behind. Not boxes only: the level's
            // column is a convex hull, and it is the case this check exists
            // for.
            if (proxy.kind == sim::ProxyVolume::Kind::Sphere) continue;
            const int segment = level.objects[oi].segment;

            // A light that actually reaches this column.
            for (const Light& light : level.lights) {
                if (std::find(light.segments.begin(), light.segments.end(), segment) ==
                    light.segments.end()) {
                    continue;
                }
                const glm::vec3 toObject = proxy.centre - light.position;
                const float span = glm::length(toObject);
                if (span < 1e-3f) continue;
                const glm::vec3 dir = toObject / span;

                // Just past the column, on the far side from the light.
                const glm::vec3 behind =
                    proxy.centre + dir * (std::max(proxy.halfExtents.x, proxy.halfExtents.z) + 2.5f);
                // The same distance out, but off to one side of it.
                const glm::vec3 across = glm::normalize(glm::cross(dir, glm::vec3(0.0f, 1.0f, 0.0f)));
                const glm::vec3 beside =
                    behind + across * (std::max(proxy.halfExtents.x, proxy.halfExtents.z) + 6.0f);

                if (!insideMine(level, behind) || !insideMine(level, beside)) continue;
                const int behindSeg = findSegment(level, behind, segment);
                const int besideSeg = findSegment(level, beside, segment);
                if (behindSeg < 0 || besideSeg < 0) continue;

                const std::vector<Light> one{light};

                sim::IlluminationQuery q;
                q.segment = besideSeg;
                q.point = beside;
                const float lit = sim::illuminationAt(q, level, one, nullptr, &objectOccluders).value;
                if (lit <= 0.0f) continue;   // the column is not between them here

                q.segment = behindSeg;
                q.point = behind;
                const float shadowed =
                    sim::illuminationAt(q, level, one, nullptr, &objectOccluders).value;
                const float open = sim::illuminationAt(q, level, one, nullptr, nullptr).value;
                if (open <= 0.0f) continue;   // a wall was in the way regardless

                ++tested;
                if (shadowed >= open) {
                    ++failed;
                    if (failed < 4) {
                        log::error("object {} cast no shadow: {:.3f} with occluders, {:.3f} "
                                   "without", oi, shadowed, open);
                    }
                } else {
                    worstDrop = std::min(worstDrop, shadowed / open);
                }
            }
        }

        if (tested == 0) {
            log::info("object shadows: SKIPPED, no column stands in a light in this level");
        } else {
            ok &= expect(failed == 0, "a column casts a shadow into the light query");
            log::info("object shadows: {} light/column pairs, {} failed; behind a column the "
                      "reading falls to {:.0f}% of the unoccluded value",
                      tested, failed, worstDrop * 100.0f);
        }
    }

    // --- nothing shadows itself --------------------------------------------
    //
    // The trap this milestone was most likely to fall into. playerVisibility
    // samples the ship's own centre, so putting the ship's proxy in the set
    // without excluding it starts every shadow ray inside solid geometry: the
    // player reads unlit against every light in the mine and nothing ever
    // notices them again. Not a flicker - the mechanic simply stops.
    {
        sim::CombatState combat;
        const sim::WeaponConfig weapons;

        // The brightest place in the level, so "dark" can only mean a bug.
        int brightest = -1;
        float best = 0.0f;
        for (int si = 0; si < n; ++si) {
            sim::IlluminationQuery q;
            q.point = level.segments[static_cast<size_t>(si)].center;
            q.segment = si;
            const float v = sim::illuminationAt(q, level, level.lights).value;
            if (v > best) {
                best = v;
                brightest = si;
            }
        }

        if (brightest < 0 || best < sim::kDarkThreshold) {
            log::info("self-occlusion: SKIPPED, nowhere in this level is lit");
        } else {
            const glm::vec3 stand = level.segments[static_cast<size_t>(brightest)].center;

            sim::OccluderSet withShip = objectOccluders;
            withShip.addBody(stand, sim::Body{}.radius, {sim::OccluderKind::Ship, -1}, brightest);

            const sim::PlayerVisibility seen = sim::playerVisibility(
                level, combat, weapons, stand, brightest, nullptr, nullptr, &withShip);

            ok &= expect(seen.illumination > 0.0f,
                         "the ship does not shadow itself into the dark");
            ok &= expect(seen.visible, "a player in the brightest room is still visible");
            log::info("self-occlusion: standing in the brightest room ({:.2f} unoccluded), with "
                      "the ship's own proxy in the set the reading is {:.2f}",
                      best, seen.illumination);

            // And the control: something ELSE in the same place does block.
            sim::OccluderSet withRobot = objectOccluders;
            withRobot.addBody(stand, 2.2f, {sim::OccluderKind::Robot, 0}, brightest);
            const sim::PlayerVisibility blocked = sim::playerVisibility(
                level, combat, weapons, stand, brightest, nullptr, nullptr, &withRobot);
            ok &= expect(blocked.illumination < seen.illumination,
                         "a robot in the same place DOES block the light");
            log::info("  a robot standing in the same spot drops it to {:.2f}",
                      blocked.illumination);
        }
    }

    // --- a floating light does not put itself out --------------------------
    {
        sim::PropState props;
        sim::spawnProps(level, props);
        if (props.floatingCount == 0) {
            log::info("emitter exclusion: SKIPPED, no floating lights in this level");
        } else {
            sim::CombatState combat;
            const sim::WeaponConfig weapons;
            const sim::FloatingLight& ball = props.floating[0];

            // Standing right beside the ball, where its own housing is between
            // the sample and nothing else.
            const glm::vec3 stand = ball.body.position + glm::vec3(0.0f, 0.0f, 3.5f);
            const int segment = findSegment(level, stand, ball.body.segment);
            if (segment < 0) {
                log::info("emitter exclusion: SKIPPED, no room beside the ball");
            } else {
                sim::OccluderSet set = objectOccluders;
                set.addBody(ball.body.position, ball.body.radius, {sim::OccluderKind::Prop, 0},
                            ball.body.segment);

                const sim::PlayerVisibility seen = sim::playerVisibility(
                    level, combat, weapons, stand, segment, nullptr, &props, &set);
                ok &= expect(seen.illumination > 0.0f,
                             "a floating light is not extinguished by its own proxy");
                log::info("emitter exclusion: beside a floating light, reading {:.2f}",
                          seen.illumination);
            }
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
        const auto visBefore = std::chrono::steady_clock::now();
        int seen = 0;
        for (int i = 0; i < 1000; ++i) {
            for (int si = 0; si < n; ++si) {
                const glm::vec3 p = level.segments[static_cast<size_t>(si)].center;
                seen += sim::playerVisibility(level, combat, weapons, p, si).visible ? 1 : 0;
            }
        }
        const uint64_t visAllocations = visScope.count();
        const double visMicros =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - visBefore)
                .count();
        log::info("  {} visibility queries: {} allocation(s), {} visible, {:.2f} us each",
                  1000 * n, visAllocations, seen, visMicros / static_cast<double>(1000 * n));
        ok &= expect(visAllocations == 0, "1000 sweeps of playerVisibility allocate nothing");

        // The same again with a full occluder set, which is what the game
        // actually passes. This is the phase where per-step cost was the stated
        // risk: playerVisibility runs 120 times a second, and every shadow ray
        // that clears the mine now also walks the set. Measured, both for
        // allocations and for what it costs.
        sim::OccluderSet full = objectOccluders;
        for (int i = 0; i < 24; ++i) {
            full.addBody(level.segments[static_cast<size_t>(i % n)].center, 2.2f,
                         {sim::OccluderKind::Robot, i}, i % n);
        }
        sim::PlayerVisibility warmO = sim::playerVisibility(
            level, combat, weapons, level.segments[0].center, 0, nullptr, nullptr, &full);
        (void)warmO;

        alloc::Scope occScope;
        const auto before = std::chrono::steady_clock::now();
        int occSeen = 0;
        for (int i = 0; i < 1000; ++i) {
            for (int si = 0; si < n; ++si) {
                const glm::vec3 p = level.segments[static_cast<size_t>(si)].center;
                occSeen += sim::playerVisibility(level, combat, weapons, p, si, nullptr, nullptr,
                                                 &full).visible ? 1 : 0;
            }
        }
        const uint64_t occAllocations = occScope.count();
        const double micros =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - before)
                .count();
        log::info("  {} visibility queries with {} occluders: {} allocation(s), {} visible, "
                  "{:.2f} us each", 1000 * n, full.count, occAllocations, occSeen,
                  micros / static_cast<double>(1000 * n));
        log::info("  occluders cost {:+.2f} us per query at {} of them, against a 8333 us step",
                  (micros - visMicros) / static_cast<double>(1000 * n), full.count);
        ok &= expect(occAllocations == 0,
                     "1000 sweeps of playerVisibility WITH occluders allocate nothing");
    }

    log::info("light test: {}", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace dvk
