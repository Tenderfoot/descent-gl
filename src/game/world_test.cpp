#include "game/world_test.h"

#include "core/log.h"
#include "game/level.h"
#include "game/visibility.h"
#include "render/camera.h"
#include "render/model.h"
#include "render/shadow.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <format>
#include <string>

namespace dvk {

namespace {

constexpr float kFovY = glm::radians(70.0f);
constexpr float kZNear = 0.05f;
constexpr float kAspect = 16.0f / 9.0f;

// Below this, a segment centre is dark enough that the player cannot make out
// the walls without a light of their own. Picked by eye against the rendered
// frames, and it only ever gates the level-design check below.
constexpr float kDarkThreshold = 0.25f;

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

} // namespace

int runWorldTest(const char* levelPath) {
    Level level = loadLevel(levelPath);   // fatal on any structural problem
    PortalWalk walk;
    bool ok = true;

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
            const float dist = glm::length(l.position - p);
            if (dist > l.radius) continue;
            const float t = dist / l.radius;
            const float window = std::clamp(1.0f - t * t * t * t, 0.0f, 1.0f);
            const float value = window * window / (1.0f + 12.0f * t * t) * l.intensity;
            if (value > best) {
                best = value;
                from = static_cast<int>(li);
            }
        }
        const bool isDark = best < kDarkThreshold;
        if (isDark) ++dark;
        log::info("  segment {:2}: {:.3f}{}{}", si, best,
                  from >= 0 ? std::format("  from light {}", from) : std::string("  unreached"),
                  isDark ? "   <- dark" : "");
    }
    log::info("  {} of {} segments are below {:.2f} and read as dark", dark, n, kDarkThreshold);

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

        std::vector<uint8_t> mask(level.segments.size(), 0);
        int totalEdges = 0;
        for (size_t i = 0; i < level.lights.size(); ++i) {
            const Light& l = level.lights[i];
            std::fill(mask.begin(), mask.end(), uint8_t{0});
            for (int seg : l.segments) mask[static_cast<size_t>(seg)] = 1;
            const int edges = gl::silhouetteEdgeCount(mineOccluder, l.position, &mask);
            totalEdges += edges;
            log::info("  light {:2}: {:3} silhouette edges on the mine walls ({} segments)", i,
                      edges, l.segments.size());
        }

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

    log::info("world test: {}", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace dvk
