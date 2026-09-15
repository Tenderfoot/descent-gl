#include "game/world_test.h"

#include "core/log.h"
#include "game/level.h"
#include "game/visibility.h"
#include "sim/gameflow.h"
#include "sim/lighting.h"
#include "sim/props.h"
#include "render/camera.h"
#include "render/model.h"
#include "render/shadow.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <format>
#include <map>
#include <string>

namespace dvk {

namespace {

constexpr float kFovY = glm::radians(70.0f);
constexpr float kZNear = 0.05f;
constexpr float kAspect = 16.0f / 9.0f;


// The six axis directions, so "what can be seen from here" does not depend on
// an arbitrary choice of facing.
constexpr std::array<glm::vec3, 6> kFacings{{
    {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
}};

glm::mat4 viewProjFrom(const glm::vec3& eye, const glm::vec3& forward) {
    const glm::vec3 up =
        std::abs(forward.y) > 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    return gl::infiniteReversedZ(kFovY, kAspect, kZNear) * glm::lookAt(eye, eye + forward, up);
}

struct Sample {
    int best = 0;    // most segments visible from this spot, over all facings
    int worst = 0;
    int lights = 0;  // lights reaching the best-case visible set
};

Sample sampleSegment(const Level& level, PortalWalk& walk, int segment) {
    Sample s;
    s.worst = level.segmentCount() + 1;
    const glm::vec3 eye = level.segments[static_cast<size_t>(segment)].center;

    for (const glm::vec3& dir : kFacings) {
        walk.build(level, segment, viewProjFrom(eye, dir), eye);
        const auto n = static_cast<int>(walk.visible().size());
        s.worst = std::min(s.worst, n);
        if (n > s.best) {
            s.best = n;
            s.lights = 0;
            for (const Light& light : level.lights) {
                const bool reaches = std::any_of(
                    light.segments.begin(), light.segments.end(),
                    [&](int seg) { return walk.isVisible(seg); });
                if (reaches) ++s.lights;
            }
        }
    }
    return s;
}

bool testPortalRevisit() {
    // Two routes reach room 3 at equal depth. The first sees only its left
    // half; the second reveals a right-hand exit to room 4. Opening a door on
    // the second route must reveal that exit without moving the camera.
    Level fixture;
    fixture.segments.resize(5);
    fixture.vertices.resize(40);
    for (int s = 0; s < 5; ++s) {
        for (int v = 0; v < 8; ++v) fixture.segments[s].verts[v] = s * 8 + v;
    }
    const auto portal = [&](int from, int side, int to, float left, float right) {
        fixture.segments[from].sides[side].neighbour = to;
        const glm::vec3 corners[4] = {{left, -0.5f, 0}, {right, -0.5f, 0},
                                      {right, 0.5f, 0}, {left, 0.5f, 0}};
        for (int c = 0; c < 4; ++c) {
            fixture.vertices[from * 8 + kSideVertices[side][c]] = corners[c];
        }
    };
    portal(0, SideBack, 1, -0.9f, -0.2f);
    portal(0, SideFront, 2, -0.9f, 0.9f);
    portal(1, SideFront, 3, -0.9f, -0.2f);
    portal(2, SideFront, 3, -0.9f, 0.9f);
    portal(3, SideFront, 4, 0.2f, 0.9f);
    sim::DoorField doors;
    doors.opaque[2 * SideCount + SideFront] = 1;
    PortalWalk walk;
    const glm::mat4 projection(1.0f);
    walk.build(fixture, 0, projection, glm::vec3(0), &doors);
    const bool shut = !walk.isVisible(4);
    doors.opaque[2 * SideCount + SideFront] = 0;
    walk.build(fixture, 0, projection, glm::vec3(0), &doors);
    const bool opened = walk.isVisible(4);
    doors.opaque[2 * SideCount + SideFront] = 1;
    walk.build(fixture, 0, projection, glm::vec3(0), &doors);
    const bool closedAgain = !walk.isVisible(4);
    log::info("portal revisit: shut {}, opened {}, closed again {}", shut, opened, closedAgain);
    return shut && opened && closedAgain;
}

bool volumeIsClosed(const std::vector<glm::vec4>& vertices) {
    // Check the emitted homogeneous geometry, not merely the input mesh.
    // Every oriented edge must be cancelled by an oppositely wound edge,
    // including finite caps, extruded sides and caps at infinity.
    using Point = std::array<float, 4>;
    std::map<std::pair<Point, Point>, int> balance;
    for (size_t t = 0; t < vertices.size(); t += 3) {
        for (size_t e = 0; e < 3; ++e) {
            const auto& a = vertices[t + e];
            const auto& b = vertices[t + (e + 1) % 3];
            Point pa{a.x, a.y, a.z, a.w}, pb{b.x, b.y, b.z, b.w};
            if (pa == pb) continue;
            if (pa < pb) ++balance[{pa, pb}];
            else --balance[{pb, pa}];
        }
    }
    return std::all_of(balance.begin(), balance.end(), [](const auto& edge) {
        return edge.second == 0;
    });
}

} // namespace

int runWorldTest(const char* levelPath) {
    Level level = loadLevel(levelPath);   // fatal on any structural problem
    PortalWalk walk;
    bool ok = testPortalRevisit();

    const int n = level.segmentCount();

    // --- lights ----------------------------------------------------------
    log::info("lights:");
    for (size_t i = 0; i < level.lights.size(); ++i) {
        const Light& l = level.lights[i];
        const bool hasOwn =
            std::find(l.segments.begin(), l.segments.end(), l.segment) != l.segments.end();
        log::info("  {:2}  segment {:2}  radius {:5.1f}  intensity {:.2f}  reaches {} segments",
                  i, l.segment, l.radius, l.intensity, l.segments.size());
        if (!hasOwn) {
            log::error("light {} does not reach its own segment {}", i, l.segment);
            ok = false;
        }
        if (l.segments.size() > static_cast<size_t>(n)) {
            log::error("light {} reaches {} segments, but the level only has {}", i,
                       l.segments.size(), n);
            ok = false;
        }
    }

    // Reach is not brightness. Nearly every segment in a mine this size is
    // inside some light's radius, but a surface at 90% of a light's radius is
    // essentially black - the falloff, not the segment set, is what makes the
    // dark stretches. So measure the thing that is actually claimed: how much
    // light arrives at each segment's centre.
    log::info("brightness at segment centres (best single light, falloff as the shader computes it):");
    int dark = 0;
    for (int si = 0; si < n; ++si) {
        const glm::vec3 p = level.segments[static_cast<size_t>(si)].center;
        float best = 0.0f;
        int from = -1;
        for (size_t li = 0; li < level.lights.size(); ++li) {
            const Light& l = level.lights[li];
            if (std::find(l.segments.begin(), l.segments.end(), si) == l.segments.end()) continue;
            const float value = sim::lightContribution(l, p);
            if (value > best) {
                best = value;
                from = static_cast<int>(li);
            }
        }
        const bool isDark = best < sim::kDarkThreshold;
        if (isDark) ++dark;
        log::info("  segment {:2}: {:.3f}{}{}", si, best,
                  from >= 0 ? std::format("  from light {}", from) : std::string("  unreached"),
                  isDark ? "   <- dark" : "");
    }
    log::info("  {} of {} segments are below {:.2f} and read as dark", dark, n, sim::kDarkThreshold);

    // A mine with no dark stretches is not this game. If every segment is
    // comfortably lit, the level is wrong even though nothing crashed.
    if (dark == 0) {
        log::error("no segment is dark: the level has no unlit stretches to hide anything in");
        ok = false;
    }

    // --- visibility ------------------------------------------------------
    log::info("portal walk, best and worst case over the six axis facings:");
    int maxVisible = 0;
    for (int si = 0; si < n; ++si) {
        const Sample s = sampleSegment(level, walk, si);
        log::info("  segment {:2}: {:2} visible best, {:2} worst, {} lights reach the view", si,
                  s.best, s.worst, s.lights);
        maxVisible = std::max(maxVisible, s.best);

        if (!walk.isVisible(si)) {
            log::error("segment {} cannot see itself", si);
            ok = false;
        }
        if (s.best > n) {
            log::error("segment {} reports {} visible segments, more than the {} that exist", si,
                       s.best, n);
            ok = false;
        }
    }

    if (maxVisible >= n) {
        log::error("every segment is visible from somewhere: portal culling is doing nothing");
        ok = false;
    } else {
        log::info("portal culling is live: at most {} of {} segments visible from any segment",
                  maxVisible, n);
    }

    // --- shadow occluders -------------------------------------------------
    //
    // buildOccluder is fatal on an open edge, so reaching the log line below at
    // all means every mesh in the level is closed. That matters more than it
    // sounds: depth-fail counting is only correct for a closed volume, and an
    // open occluder produces a shadow that flickers or inverts with the camera
    // rather than one that is obviously missing.
    log::info("shadow occluders:");
    {
        std::vector<glm::uvec3> triangles;
        std::vector<int> segments;
        triangles.reserve(level.walls.size());
        segments.reserve(level.walls.size());
        for (const WallTriangle& w : level.walls) {
            triangles.emplace_back(static_cast<unsigned>(w.vi[0]), static_cast<unsigned>(w.vi[1]),
                                   static_cast<unsigned>(w.vi[2]));
            segments.push_back(w.segment);
        }
        const gl::OccluderMesh mine =
            gl::buildOccluder(level.vertices, triangles, segments, "mine walls");

        // Euler characteristic 2 says the union of every segment's walls is one
        // closed surface of genus 0 - a sphere - which is exactly the claim the
        // segment-graph format makes: portals are interior, walls are the
        // boundary, and the two sides of a portal share their vertices.
        const int euler = static_cast<int>(mine.positions.size()) -
                          static_cast<int>(mine.edges.size()) +
                          static_cast<int>(mine.triangles.size());
        log::info("  mine walls: V {} - E {} + F {} = {}", mine.positions.size(),
                  mine.edges.size(), mine.triangles.size(), euler);
        if (euler != 2) {
            log::error("the mine's wall surface has Euler characteristic {}, not 2: it is not one "
                       "closed surface, and shadow volumes will be wrong somewhere",
                       euler);
            ok = false;
        }

        gl::Occluder mineOccluder;
        mineOccluder.reset(&mine, glm::mat4(1.0f));

        std::vector<glm::vec4> volume;
        std::vector<uint8_t> faces;
        int totalEdges = 0;
        for (size_t i = 0; i < level.lights.size(); ++i) {
            const Light& l = level.lights[i];
            volume.clear();
            const int edges = gl::appendShadowVolume(mineOccluder, l.position, volume, faces);
            if (volume.empty() || !volumeIsClosed(volume)) {
                log::error("light {} produced an open shadow volume", i);
                ok = false;
            }
            totalEdges += edges;
            log::info("  light {:2}: {:3} silhouette edges on the mine walls ({} segments)", i,
                      edges, l.segments.size());
        }
        log::info("  checked generated volume closure for {} lights", level.lights.size());

        // A light with no silhouette edges anywhere casts nothing from the
        // walls, which in a mine with bends means the extraction is broken.
        if (totalEdges == 0) {
            log::error("no light produces a single silhouette edge on the mine walls: "
                       "silhouette extraction is not working");
            ok = false;
        } else {
            log::info("  {} silhouette edges in total across {} lights", totalEdges,
                      level.lights.size());
        }
    }

    for (const LevelObject& obj : level.objects) {
        const std::string path = "assets/models/" + obj.type + ".obj";
        static std::vector<std::string> seen;
        if (std::find(seen.begin(), seen.end(), path) != seen.end()) continue;
        seen.push_back(path);
        const gl::ModelData model = gl::loadObj(path);
        gl::buildOccluder(model.positions, model.triangles, {}, path);   // fatal if open
    }
    {
        const gl::ModelData ship = gl::loadObj("assets/models/ship.obj");
        gl::buildOccluder(ship.positions, ship.triangles, {}, "assets/models/ship.obj");
    }

    // --- editor round trip -----------------------------------------------
    //
    // The ImGui light editor's Save button writes through saveLevel. If that
    // output does not load back, the way anyone finds out is by editing their
    // lights, saving, and losing the level. Cheap to check here instead.
    {
        const std::string temp = std::string(levelPath) + ".roundtrip.tmp";
        if (!saveLevel(temp, level)) {
            log::error("saveLevel failed, so the light editor's Save button would too");
            ok = false;
        } else {
            const Level reloaded = loadLevel(temp);   // fatal if it does not parse
            if (reloaded.segmentCount() != n ||
                reloaded.lights.size() != level.lights.size() ||
                reloaded.walls.size() != level.walls.size()) {
                log::error("round trip changed the level: {} segments / {} lights / {} walls "
                           "became {} / {} / {}",
                           n, level.lights.size(), level.walls.size(), reloaded.segmentCount(),
                           reloaded.lights.size(), reloaded.walls.size());
                ok = false;
            } else {
                log::info("save/reload round trip preserves {} segments, {} lights, {} walls", n,
                          level.lights.size(), level.walls.size());
            }
            std::remove(temp.c_str());
        }
    }

    // --- the props that move --------------------------------------------
    //
    // A floating light has to survive being shot hard in any direction: it
    // bounces rather than slides, and restitution inside an iterated push-out
    // is exactly the case that can throw a body through a corner. A lamp has to
    // stay on its cord and inside the room it hangs in.
    if (!level.floatingLights.empty() || !level.swingingLights.empty()) {
        constexpr float kStep = 1.0f / 120.0f;
        sim::PropState props;
        sim::spawnProps(level, props);

        int escapes = 0;
        int bounced = 0;
        int trials = 0;
        for (int fi = 0; fi < props.floatingCount; ++fi) {
            for (int dx = -1; dx <= 1; ++dx) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dz = -1; dz <= 1; ++dz) {
                        if (dx == 0 && dy == 0 && dz == 0) continue;
                        ++trials;

                        sim::PropState run = props;
                        sim::FloatingLight& ball = run.floating[static_cast<size_t>(fi)];
                        const glm::vec3 shove =
                            glm::normalize(glm::vec3(dx, dy, dz)) * 150.0f;   // past any weapon
                        ball.body.knockback = shove;

                        bool reversed = false;
                        for (int step = 0; step < 600; ++step) {   // five seconds
                            sim::stepProps(level, run, kStep);
                            if (!insideMine(level, ball.body.position)) {
                                if (escapes < 4) {
                                    log::error("floating light {} left the mine heading {} {} {} "
                                               "at {:.2f}s, position {:.2f} {:.2f} {:.2f}",
                                               fi, dx, dy, dz, static_cast<float>(step) * kStep,
                                               ball.body.position.x, ball.body.position.y,
                                               ball.body.position.z);
                                }
                                ++escapes;
                                break;
                            }
                            if (glm::dot(ball.body.motion(), shove) < 0.0f) reversed = true;
                        }
                        if (reversed) ++bounced;
                    }
                }
            }
        }

        if (escapes > 0) {
            log::error("{} of {} floating-light trials left the mine", escapes, trials);
            ok = false;
        }
        if (trials > 0 && bounced < trials) {
            log::error("only {} of {} floating-light trials bounced back off a wall", bounced,
                       trials);
            ok = false;
        }
        if (trials > 0) {
            log::info("floating lights: {} trials at 150 u/s for 5 s, {} escapes, {} bounced back",
                      trials, escapes, bounced);
        }

        // One full period of every lamp: it must reach its authored amplitude
        // on both sides, stay exactly a cord's length from its anchor, and
        // never swing out of the mine.
        for (int li = 0; li < props.swingingCount; ++li) {
            sim::PropState run = props;
            sim::SwingingLight& lamp = run.swinging[static_cast<size_t>(li)];
            const float period = 6.2831853f / std::max(0.01f, lamp.rate);
            float widest = 0.0f;
            float worstCord = 0.0f;
            int outside = 0;

            for (int step = 0; step * kStep < period * 1.05f; ++step) {
                sim::stepProps(level, run, kStep);
                const glm::vec3 arm = lamp.position - lamp.anchor;
                worstCord = std::max(worstCord, std::abs(glm::length(arm) - lamp.cordLength));
                widest = std::max(widest, std::acos(std::clamp(
                                              glm::dot(glm::normalize(arm),
                                                       glm::vec3(0.0f, -1.0f, 0.0f)),
                                              -1.0f, 1.0f)));
                if (!insideMine(level, lamp.position)) ++outside;
            }

            if (outside > 0) {
                log::error("swinging light {} left the mine on {} steps", li, outside);
                ok = false;
            }
            if (worstCord > 0.001f) {
                log::error("swinging light {} stretched its cord by {:.4f}u", li, worstCord);
                ok = false;
            }
            if (std::abs(widest - lamp.amplitude) > 0.02f) {
                log::error("swinging light {} reached {:.3f} rad, not its {:.3f} amplitude", li,
                           widest, lamp.amplitude);
                ok = false;
            }
            log::info("swinging light {}: {:.2f}s period, reached {:.3f} rad of {:.3f}, cord held "
                      "to {:.5f}u", li, period, widest, lamp.amplitude, worstCord);
        }
    }

    log::info("world test: {}", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace dvk
