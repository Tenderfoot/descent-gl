#include "game/world_test.h"

#include "core/log.h"
#include "game/level.h"
#include "game/visibility.h"
#include "render/camera.h"

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
