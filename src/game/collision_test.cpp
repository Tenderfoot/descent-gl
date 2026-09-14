#include "game/collision_test.h"

#include "core/log.h"
#include "game/level.h"
#include "sim/collision.h"
#include "sim/flight.h"
#include "sim/geometry.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace dvk {

namespace {

constexpr float kStep = 1.0f / 120.0f;

// The 26 directions through a cube's faces, edges and corners. Fixed and
// deterministic, and they cover the cases that matter: straight at a wall,
// straight at an edge, straight at a corner.
std::array<glm::vec3, 26> makeDirections() {
    std::array<glm::vec3, 26> dirs{};
    size_t n = 0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            for (int z = -1; z <= 1; ++z) {
                if (x == 0 && y == 0 && z == 0) continue;
                dirs[n++] = glm::normalize(glm::vec3(x, y, z));
            }
        }
    }
    return dirs;
}

// Is this point inside the mine at all?
//
// Deliberately NOT findSegment, which walks the segment graph using one
// averaged plane per side. That is the cheap traditional test and it is
// approximate for a side that is not planar - and the whole point of the format
// is that sides are not planar. Using it as the oracle here would measure the
// approximation rather than the collision.
//
// Also deliberately not "nearest wall triangle, which side of it". That one is
// wrong at a convex edge - where the mine's surface folds towards the interior,
// as it does at the inside of every bend. Beyond such an edge the nearest point
// is ON the edge, and the direction back to the query point has a negative dot
// with both adjacent faces' normals, so an interior point reads as outside. It
// reported nine false escapes and two false raycast disagreements before this
// was replaced.
//
// Ray parity instead: the union of every segment's wall triangles is one closed
// surface (validated in M3), so a ray from an interior point crosses it an odd
// number of times. Three directions and a majority vote, because a single ray
// that grazes an edge can double-count it.
bool insideMine(const Level& level, const glm::vec3& p) {
    // Deliberately irrational-looking directions: a ray down an axis lies in
    // the plane of a grid-aligned wall far too often.
    static const std::array<glm::vec3, 3> kProbes{{
        glm::normalize(glm::vec3(0.5773f, 0.3313f, 0.7454f)),
        glm::normalize(glm::vec3(-0.8112f, 0.4231f, 0.4037f)),
        glm::normalize(glm::vec3(0.2671f, -0.8923f, 0.3629f)),
    }};

    int votes = 0;
    for (const glm::vec3& dir : kProbes) {
        int crossings = 0;
        for (const WallTriangle& w : level.walls) {
            if (sim::rayTriangleDistance(p, dir, w.p[0], w.p[1], w.p[2]) > 1e-4f) ++crossings;
        }
        if (crossings % 2 == 1) ++votes;
    }
    return votes >= 2;
}

} // namespace

int runCollisionTest(const char* levelPath, const char* flightConfigPath) {
    const Level level = loadLevel(levelPath);
    const sim::FlightConfig flight = sim::loadFlightConfig(flightConfigPath);
    const std::array<glm::vec3, 26> dirs = makeDirections();

    sim::CollisionConfig collision;
    collision.scrapeDamping = flight.wallScrape;

    bool ok = true;
    const int n = level.segmentCount();

    // --- containment -----------------------------------------------------
    //
    // Full throttle is re-applied every step rather than integrated, so the
    // body is pressed into the wall continuously instead of bouncing off once.
    // That is the case that finds a leak.
    {
        constexpr int kSteps = 360;   // three seconds of sustained contact
        int escapes = 0;
        int worstStep = kSteps;
        for (int si = 0; si < n; ++si) {
            for (const glm::vec3& dir : dirs) {
                sim::Body body;
                body.position = level.segments[static_cast<size_t>(si)].center;
                body.segment = si;
                body.radius = flight.shipRadius;

                for (int step = 0; step < kSteps; ++step) {
                    body.velocity = dir * flight.maxSpeed;
                    body.position += body.velocity * kStep;
                    sim::resolveCollision(level, body, collision, kStep, nullptr);

                    if (!insideMine(level, body.position)) {
                        if (escapes < 6) {
                            log::error("escaped the mine: from segment {} heading {:.2f} {:.2f} "
                                       "{:.2f}, at step {} ({:.2f}s), position {:.2f} {:.2f} "
                                       "{:.2f}",
                                       si, dir.x, dir.y, dir.z, step,
                                       static_cast<float>(step) * kStep, body.position.x,
                                       body.position.y, body.position.z);
                        }
                        ++escapes;
                        worstStep = std::min(worstStep, step);
                        break;
                    }
                }
            }
        }
        const int trials = n * static_cast<int>(dirs.size());
        log::info("containment: {} trials ({} segments x {} directions x {:.1f}s at {:.0f} u/s), "
                  "{} escapes",
                  trials, n, dirs.size(), static_cast<float>(kSteps) * kStep, flight.maxSpeed,
                  escapes);
        if (escapes > 0) {
            log::error("{} of {} trials left the mine (first at step {})", escapes, trials,
                       worstStep);
            ok = false;
        }
    }

    // --- sliding ---------------------------------------------------------
    //
    // Pressed diagonally into a wall, a body should keep most of its speed
    // along the wall rather than stopping dead or juddering. Measured as
    // distance actually covered over one second.
    {
        int stuck = 0;
        int tested = 0;
        for (int si = 0; si < n; ++si) {
            for (int side = 0; side < SideCount; ++side) {
                const SideData& sd = level.segments[static_cast<size_t>(si)].sides[static_cast<size_t>(side)];
                if (sd.isPortal()) continue;

                // Into the wall at 45 degrees: half the push is along it.
                const glm::vec3 into = -sd.normal;
                glm::vec3 along = glm::cross(sd.normal, glm::vec3(0.0f, 1.0f, 0.0f));
                if (glm::length(along) < 0.1f) along = glm::cross(sd.normal, glm::vec3(1.0f, 0.0f, 0.0f));
                along = glm::normalize(along);
                const glm::vec3 dir = glm::normalize(into + along);

                sim::Body body;
                body.position = level.segments[static_cast<size_t>(si)].center;
                body.segment = si;
                body.radius = flight.shipRadius;
                const glm::vec3 start = body.position;

                for (int step = 0; step < 120; ++step) {
                    body.velocity = dir * flight.maxSpeed;
                    body.position += body.velocity * kStep;
                    sim::resolveCollision(level, body, collision, kStep, nullptr);
                }

                ++tested;
                const float covered = glm::length(body.position - start);
                // Free flight would cover maxSpeed units in a second. Half of
                // the push is into the wall, so a body that slides properly
                // still covers a good fraction of it; one that catches on the
                // wall covers almost nothing.
                if (covered < flight.maxSpeed * 0.25f) {
                    if (stuck < 4) {
                        log::error("stuck sliding: segment {} side {}, covered only {:.2f} units "
                                   "in one second",
                                   si, kSideNames[static_cast<size_t>(side)], covered);
                    }
                    ++stuck;
                }
            }
        }
        log::info("sliding: {} walls tested at 45 degrees, {} caught", tested, stuck);
        if (stuck > 0) ok = false;
    }

    // --- portals both ways ------------------------------------------------
    {
        int portals = 0;
        int failed = 0;
        for (int si = 0; si < n; ++si) {
            const Segment& seg = level.segments[static_cast<size_t>(si)];
            for (int side = 0; side < SideCount; ++side) {
                const SideData& sd = seg.sides[static_cast<size_t>(side)];
                if (!sd.isPortal()) continue;
                ++portals;

                sim::Body body;
                body.position = seg.center;
                body.segment = si;
                body.radius = flight.shipRadius;
                const glm::vec3 dir = glm::normalize(sd.center - seg.center);

                bool arrived = false;
                for (int step = 0; step < 480 && !arrived; ++step) {
                    body.velocity = dir * flight.maxSpeed;
                    body.position += body.velocity * kStep;
                    sim::resolveCollision(level, body, collision, kStep, nullptr);
                    if (body.segment == sd.neighbour) arrived = true;
                }
                if (!arrived) {
                    log::error("could not fly from segment {} through its {} portal into {}", si,
                               kSideNames[static_cast<size_t>(side)], sd.neighbour);
                    ++failed;
                }
            }
        }
        // Every portal is counted from both of its sides, so this is both
        // directions by construction.
        log::info("portals: {} crossings attempted (each portal from both sides), {} failed",
                  portals, failed);
        if (failed > 0) ok = false;
    }

    // --- raycast, against an independent method ---------------------------
    //
    // Marching a point in small increments until it leaves the mine is a slow
    // and obviously-correct way to find the same wall. If the two disagree, one
    // of them is wrong, and it is worth knowing which before M5 starts firing
    // things along the fast one.
    {
        constexpr float kMarchStep = 0.05f;
        constexpr float kTolerance = 0.35f;
        constexpr float kMaxDistance = 400.0f;
        int tested = 0;
        int misses = 0;
        int disagreements = 0;
        float worst = 0.0f;

        for (int si = 0; si < n; ++si) {
            const glm::vec3 origin = level.segments[static_cast<size_t>(si)].center;
            for (const glm::vec3& dir : dirs) {
                ++tested;
                const sim::RayHit hit = sim::raycast(level, origin, dir, kMaxDistance, si);
                if (!hit.hit) {
                    log::error("raycast from segment {} heading {:.2f} {:.2f} {:.2f} hit nothing, "
                               "but the mine is closed",
                               si, dir.x, dir.y, dir.z);
                    ++misses;
                    continue;
                }
                if (hit.segment < 0 || hit.segment >= n) {
                    log::error("raycast reported segment {}, which does not exist", hit.segment);
                    ++disagreements;
                    continue;
                }

                float marched = 0.0f;
                while (marched < kMaxDistance && insideMine(level, origin + dir * marched)) {
                    marched += kMarchStep;
                }

                const float delta = std::abs(marched - hit.distance);
                worst = std::max(worst, delta);
                if (delta > kTolerance) {
                    if (disagreements < 4) {
                        log::error("raycast says {:.3f} units from segment {} heading {:.2f} "
                                   "{:.2f} {:.2f}, marching says {:.3f}",
                                   hit.distance, si, dir.x, dir.y, dir.z, marched);
                    }
                    ++disagreements;
                }
            }
        }
        log::info("raycast: {} rays, {} missed, {} disagreed with marching (worst {:.3f} units, "
                  "tolerance {:.2f})",
                  tested, misses, disagreements, worst, kTolerance);
        if (misses > 0 || disagreements > 0) ok = false;
    }

    // --- swept sphere ------------------------------------------------------
    //
    // A sphere must stop no later than a ray along the same line: it has a
    // radius, so it meets the wall first. It can stop earlier by clipping
    // something the centre line misses, which is the whole point.
    {
        int tested = 0;
        int wrong = 0;
        int earlier = 0;
        for (int si = 0; si < n; ++si) {
            const glm::vec3 origin = level.segments[static_cast<size_t>(si)].center;
            for (const glm::vec3& dir : dirs) {
                ++tested;
                const sim::RayHit ray = sim::raycast(level, origin, dir, 400.0f, si);
                const sim::RayHit sweep =
                    sim::sweepSphere(level, origin, dir, flight.shipRadius, 400.0f, si);
                if (!sweep.hit) {
                    log::error("swept sphere from segment {} heading {:.2f} {:.2f} {:.2f} found "
                               "nothing, but the ray hit at {:.2f}",
                               si, dir.x, dir.y, dir.z, ray.distance);
                    ++wrong;
                    continue;
                }
                if (sweep.distance > ray.distance + 0.01f) {
                    log::error("swept sphere travelled {:.3f} but the ray stopped at {:.3f}: the "
                               "sphere passed through a wall the ray hit",
                               sweep.distance, ray.distance);
                    ++wrong;
                } else if (sweep.distance < ray.distance - 0.01f) {
                    ++earlier;
                }
            }
        }
        log::info("swept sphere: {} sweeps, {} stopped short of the ray as expected, {} wrong",
                  tested, earlier, wrong);
        if (wrong > 0) ok = false;
    }

    log::info("collision test: {}", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace dvk
