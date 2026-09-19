#include "game/collision_test.h"

#include "core/log.h"
#include "game/level.h"
#include "sim/collision.h"
#include "sim/flight.h"
#include "sim/geometry.h"
#include "sim/proxy.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace dvk {

namespace {

constexpr float kStep = 1.0f / 120.0f;

bool expect(bool condition, const char* what) {
    if (!condition) log::error("FAILED: {}", what);
    return condition;
}

// Ignore an intersection at the ray's own origin. The origins here are segment
// centres, so nothing legitimate sits this close.
constexpr float kHitEpsilon = 1.0e-4f;

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

// Where the ray meets the mine's surface, by testing every wall triangle in the
// level and keeping the nearest. The union of all wall triangles is one closed
// surface (validated in M3), so the first triangle a ray from an interior point
// meets is the wall it hits. That makes this exact: no stepping, no threshold to
// tune, and no segment graph - which is what keeps it independent of the thing
// it checks, since raycast's whole job is to walk that graph.
//
// This replaced a marching oracle that stepped a point along the ray until it
// left the mine. Marching cannot tell passing through a corner from passing
// near one, and both answers it was given were wrong. With no clearance
// requirement it walked through the single point where two segments meet at a
// grid diagonal. With one (M7 added it, 0.05 units) it stopped at any near
// graze: on mine01 a ray from segment 16's centre along (-0.71, 0, -0.71)
// clears a portal corner by 0.0202 units and carries on into segment 14, but
// the marched oracle called that a wall at 13.850 units and so reported
// raycast's correct 71.023 as the milestone's one outstanding failure.
float firstWallHit(const Level& level, const glm::vec3& origin, const glm::vec3& dir) {
    float best = std::numeric_limits<float>::infinity();
    for (const WallTriangle& w : level.walls) {
        const float t = sim::rayTriangleDistance(origin, dir, w.p[0], w.p[1], w.p[2]);
        if (t > kHitEpsilon && t < best) best = t;
    }
    return best;
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

    // --- containment and energy under knockback ---------------------------
    //
    // The knockback channel deliberately sits outside maxSpeed, so a blast can
    // push far harder than the ship's own engines ever do. Two things have to
    // hold at that speed: the mine still contains the body, and a bouncing body
    // never leaves a collision faster than it arrived - restitution inside an
    // iterated push-out can otherwise compound in a corner and run away.
    {
        constexpr int kSteps = 360;
        constexpr float kBlastSpeed = 150.0f;   // well past anything the weapons produce
        int escapes = 0;
        float fastest = 0.0f;

        for (const float restitution : {0.0f, 0.65f}) {
            sim::CollisionConfig bouncy = collision;
            bouncy.restitution = restitution;
            // A bouncing body wants no scrape: otherwise the bounce is bled
            // away as fast as it is made, and the case never gets tested.
            if (restitution > 0.0f) bouncy.scrapeDamping = 0.0f;

            for (int si = 0; si < n; ++si) {
                for (const glm::vec3& dir : dirs) {
                    sim::Body body;
                    body.position = level.segments[static_cast<size_t>(si)].center;
                    body.segment = si;
                    body.radius = flight.shipRadius;
                    body.knockback = dir * kBlastSpeed;

                    for (int step = 0; step < kSteps; ++step) {
                        body.knockback *= std::exp(-sim::kKnockbackDamping * kStep);
                        body.position += body.motion() * kStep;
                        sim::resolveCollision(level, body, bouncy, kStep, nullptr);
                        fastest = std::max(fastest, glm::length(body.motion()));

                        if (!insideMine(level, body.position)) {
                            if (escapes < 6) {
                                log::error("knockback escaped the mine: segment {} heading "
                                           "{:.2f} {:.2f} {:.2f}, restitution {:.2f}, step {}",
                                           si, dir.x, dir.y, dir.z, restitution, step);
                            }
                            ++escapes;
                            break;
                        }
                    }
                }
            }
        }

        log::info("knockback: {} u/s in both slide and bounce, {} escapes, fastest seen {:.1f} u/s",
                  kBlastSpeed, escapes, fastest);
        if (escapes > 0) {
            log::error("{} knockback trials left the mine", escapes);
            ok = false;
        }
        if (fastest > kBlastSpeed + 0.5f) {
            log::error("a bounce manufactured speed: {:.1f} u/s from {:.1f}", fastest, kBlastSpeed);
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
    // Testing the ray against every wall triangle in the level is a slow and
    // obviously-correct way to find the same wall. If the two disagree, one of
    // them is wrong, and it is worth knowing which before M5 starts firing
    // things along the fast one.
    //
    // The tolerance is float noise rather than a fudge factor: both methods
    // intersect the same triangles with the same routine, so the only
    // difference is the order they are visited in and the origin each test
    // starts from. Anything larger than this is a real disagreement.
    {
        constexpr float kTolerance = 1.0e-3f;
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

                const float brute = firstWallHit(level, origin, dir);
                if (!std::isfinite(brute)) {
                    log::error("no wall triangle anywhere along the ray from segment {} heading "
                               "{:.2f} {:.2f} {:.2f}, but the mine is closed",
                               si, dir.x, dir.y, dir.z);
                    ++disagreements;
                    continue;
                }

                const float delta = std::abs(brute - hit.distance);
                worst = std::max(worst, delta);
                if (delta > kTolerance) {
                    if (disagreements < 4) {
                        log::error("raycast says {:.3f} units from segment {} heading {:.2f} "
                                   "{:.2f} {:.2f}, every-triangle says {:.3f}",
                                   hit.distance, si, dir.x, dir.y, dir.z, brute);
                    }
                    ++disagreements;
                }
            }
        }
        log::info("raycast: {} rays, {} missed, {} disagreed with every-triangle (worst {:.6f} "
                  "units, tolerance {:.6f})",
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

    // ====================================================================
    // Bodies against each other
    // ====================================================================

    // The roomiest segment in the level, so a two-body test has space to run
    // without the walls taking part in it. Measured, not assumed: the invariants
    // below are only meaningful while nothing else is touching.
    int openSegment = 0;
    float openRoom = 0.0f;
    for (int si = 0; si < n; ++si) {
        const glm::vec3 centre = level.segments[static_cast<size_t>(si)].center;
        float nearest = std::numeric_limits<float>::infinity();
        for (const glm::vec3& dir : dirs) nearest = std::min(nearest, firstWallHit(level, centre, dir));
        if (nearest > openRoom) {
            openRoom = nearest;
            openSegment = si;
        }
    }
    const glm::vec3 openCentre = level.segments[static_cast<size_t>(openSegment)].center;
    log::info("open space: segment {} has {:.1f} units of clear room in every direction",
              openSegment, openRoom);

    // --- two bodies cannot end a step inside each other -------------------
    {
        constexpr int kSteps = 240;
        int worstOverlap = 0;
        float deepest = 0.0f;

        for (const glm::vec3& dir : dirs) {
            sim::Body a;
            sim::Body b;
            a.radius = flight.shipRadius;
            b.radius = 1.9f;            // a drone
            a.mass = 1.5f;              // the player
            b.mass = 0.8f;
            a.segment = openSegment;
            b.segment = openSegment;
            const float apart = (a.radius + b.radius) * 1.6f;
            a.position = openCentre - dir * apart;
            b.position = openCentre + dir * apart;

            sim::BodySet set;
            set.add(a);
            set.add(b);

            for (int step = 0; step < kSteps; ++step) {
                // Driven at each other every step, so they are pressed together
                // rather than bouncing apart once.
                a.velocity = dir * flight.maxSpeed;
                b.velocity = -dir * flight.maxSpeed;
                a.position += a.motion() * kStep;
                b.position += b.motion() * kStep;
                // The real step order: each system resolves its own body
                // against the mine, then the shared pass settles the contact
                // between them. resolveBodies is a correction, not the primary
                // containment - see main.cpp.
                sim::resolveCollision(level, a, collision, kStep, nullptr);
                sim::resolveCollision(level, b, collision, kStep, nullptr);
                sim::resolveBodies(level, set, collision, kStep, nullptr, nullptr);

                const float gap = glm::length(b.position - a.position) - (a.radius + b.radius);
                if (gap < -1e-3f) {
                    ++worstOverlap;
                    deepest = std::min(deepest, gap);
                }
            }
        }
        ok &= expect(worstOverlap == 0, "two bodies never end a step inside each other");
        log::info("body overlap: 26 head-on approaches x {} steps, {} overlapping ends (deepest "
                  "{:.4f})", kSteps, worstOverlap, deepest);
    }

    // --- what is actually conserved ---------------------------------------
    //
    // The first draft of this phase proposed asserting that total speed never
    // grows. That is true against a wall, which is effectively infinite mass,
    // and FALSE between two bodies of different mass: 100 into 1 at rest,
    // elastically, takes the sum of speeds from 1 to 2.96 while kinetic energy
    // does not move. So the invariants are the real ones - momentum conserved,
    // kinetic energy not increasing - and this case measures the speed sum too,
    // to show why it was the wrong thing to assert.
    {
        sim::Body heavy;
        sim::Body light;
        heavy.radius = 2.0f;
        light.radius = 1.0f;
        heavy.mass = 100.0f;
        light.mass = 1.0f;
        heavy.segment = openSegment;
        light.segment = openSegment;

        const glm::vec3 dir(1.0f, 0.0f, 0.0f);
        // Just touching, closing at 1 u/s.
        light.position = openCentre;
        heavy.position = openCentre - dir * (heavy.radius + light.radius - 0.01f);
        heavy.velocity = dir * 1.0f;

        sim::BodySet set;
        set.add(heavy, 1.0f);   // elastic, so energy is conserved rather than lost
        set.add(light, 1.0f);

        const auto momentum = [&] {
            return heavy.mass * heavy.motion() + light.mass * light.motion();
        };
        const auto energy = [&] {
            return 0.5f * heavy.mass * glm::dot(heavy.motion(), heavy.motion()) +
                   0.5f * light.mass * glm::dot(light.motion(), light.motion());
        };
        const auto speedSum = [&] {
            return glm::length(heavy.motion()) + glm::length(light.motion());
        };

        const glm::vec3 p0 = momentum();
        const float e0 = energy();
        const float s0 = speedSum();

        sim::resolveBodies(level, set, collision, kStep, nullptr, nullptr);

        const glm::vec3 p1 = momentum();
        const float e1 = energy();
        const float s1 = speedSum();

        const float momentumDrift = glm::length(p1 - p0) / std::max(1e-6f, glm::length(p0));
        ok &= expect(momentumDrift < 1e-3f, "momentum is conserved for an isolated pair");
        ok &= expect(e1 <= e0 * 1.001f, "kinetic energy does not grow");

        log::info("100:1 elastic contact - momentum {:.4f} -> {:.4f} (drift {:.2e}), energy "
                  "{:.4f} -> {:.4f}, speed sum {:.3f} -> {:.3f}",
                  glm::length(p0), glm::length(p1), momentumDrift, e0, e1, s0, s1);
        log::info("  the speed sum {} - which is why 'total speed never grows' is not the "
                  "invariant to test", s1 > s0 + 1e-3f ? "GREW, as the physics requires"
                                                       : "did not grow here");
    }

    // --- pushing bodies together never pushes one out of the mine ---------
    //
    // The ordering rule - body against body first, the mine last - exists for
    // this. Two bodies pressed into the same corner want to separate along a
    // line that leaves the room; the wall pass has to get the final say.
    {
        int escapes = 0;
        int tested = 0;
        for (int si = 0; si < n; ++si) {
            for (const glm::vec3& dir : dirs) {
                sim::Body a;
                sim::Body b;
                a.radius = flight.shipRadius;
                b.radius = flight.shipRadius;
                a.mass = 1.5f;
                b.mass = 1.5f;
                a.position = level.segments[static_cast<size_t>(si)].center;
                b.position = a.position + dir * 0.35f;   // already overlapping
                a.segment = si;
                b.segment = si;

                sim::BodySet set;
                set.add(a);
                set.add(b);

                ++tested;
                for (int step = 0; step < 120; ++step) {
                    // Both driven into the same corner, so the wall correction
                    // and the body separation fight each other every step.
                    a.velocity = dir * flight.maxSpeed;
                    b.velocity = dir * flight.maxSpeed;
                    a.position += a.motion() * kStep;
                    b.position += b.motion() * kStep;
                    sim::resolveCollision(level, a, collision, kStep, nullptr);
                    sim::resolveCollision(level, b, collision, kStep, nullptr);
                    sim::resolveBodies(level, set, collision, kStep, nullptr, nullptr);
                }
                if (!insideMine(level, a.position) || !insideMine(level, b.position)) {
                    if (escapes < 6) {
                        log::error("body pair escaped from segment {} heading {:.2f} {:.2f} {:.2f}",
                                   si, dir.x, dir.y, dir.z);
                    }
                    ++escapes;
                }
            }
        }
        ok &= expect(escapes == 0, "separating two bodies never pushes one out of the mine");
        log::info("pair containment: {} corner cases, {} escapes", tested, escapes);
    }

    // The column's own radius at a height in its own space, whichever shape the
    // loader gave it. A box around a SQUARE column is a perfect fit and reads
    // its half width here; a box around a round one reads the widest it ever
    // is, and the gap at its corners is exactly the bug. Shared by the two
    // checks below so neither can quietly stop testing the shape it was given.
    const auto columnRadiusAt = [](const sim::PlacedProxy& proxy, float y) {
        return proxy.kind == sim::ProxyVolume::Kind::TaperedCylinder
                   ? sim::taperedRadiusAt(proxy.radii, proxy.halfExtents.y, y)
                   : std::max(proxy.halfExtents.x, proxy.halfExtents.z);
    };

    // --- a body cannot enter a level object -------------------------------
    //
    // And how far off the visible surface it stops, which is the honest number:
    // the proxy is the model's bounding box, and a box around a tapered
    // octagonal column contains empty space. Reported rather than asserted,
    // because it is a known cost of the box and the figure is what tells anyone
    // whether it has become intolerable.
    {
        int tested = 0;
        int entered = 0;
        int settled = 0;
        int tapered = 0;
        float worstGap = 0.0f;

        // Everything is solid for this test: no robots in the way of the point.
        std::array<bool, 64> alive{};
        alive.fill(true);
        const sim::SolidObjects solids{alive.data(),
                                       std::min<int>(64, static_cast<int>(level.objects.size()))};

        for (size_t oi = 0; oi < level.objectProxies.size() && oi < 64; ++oi) {
            const sim::PlacedProxy& proxy = level.objectProxies[oi];
            if (!sim::proxyIsColumn(proxy)) continue;
            const int segment = level.objects[oi].segment;

            for (const glm::vec3& dir : dirs) {
                sim::Body body;
                body.radius = flight.shipRadius;
                body.mass = 1.5f;
                body.segment = segment;
                body.position = proxy.centre - dir * 12.0f;
                if (!insideMine(level, body.position)) continue;
                body.segment = findSegment(level, body.position, segment);
                if (body.segment < 0) continue;

                sim::BodySet set;
                set.add(body);
                ++tested;

                for (int step = 0; step < 240; ++step) {
                    body.velocity = dir * flight.maxSpeed;
                    body.position += body.motion() * kStep;
                    sim::resolveCollision(level, body, collision, kStep, nullptr);
                    sim::resolveBodies(level, set, collision, kStep, nullptr, &solids);
                }

                // Inside the box at all?
                sim::ProxyContact contact;
                if (sim::proxySphereContact(proxy, body.position, 0.0f, contact)) {
                    if (entered < 4) {
                        log::error("a body ended up inside object {} heading {:.2f} {:.2f} {:.2f}",
                                   oi, dir.x, dir.y, dir.z);
                    }
                    ++entered;
                    continue;
                }
                // Only a body that actually came to rest against the box says
                // anything about the box's fit. One that stopped against a wall
                // on the way in is measuring the room, not the column.
                if (!sim::proxySphereContact(proxy, body.position, body.radius + 0.25f, contact)) {
                    continue;
                }
                ++settled;
                // How far it sits from the column's own surface. The column is
                // a solid of revolution about its own Y, so compare against its
                // radius AT THIS HEIGHT - against the widest it ever is, which
                // is what this measured while the proxy was a box, a taper
                // reads as a gap that is really the model getting narrower.
                // Only meaningful beside the column, not above or below its
                // caps.
                const glm::vec3 local =
                    glm::conjugate(proxy.orientation) * (body.position - proxy.centre);
                if (std::abs(local.y) < proxy.halfExtents.y - body.radius) {
                    const float radial = glm::length(glm::vec2(local.x, local.z));
                    worstGap = std::max(worstGap, radial - body.radius -
                                                      columnRadiusAt(proxy, local.y));
                    ++tapered;
                }
            }
        }

        if (tested == 0) {
            log::info("object collision: SKIPPED, no column-shaped object in this level");
        } else {
            ok &= expect(entered == 0, "a body never ends up inside a level object");
            log::info("object collision: {} approaches, {} came to rest on the object, {} "
                      "entered it; worst standoff past the column's own radius at that height "
                      "{:.2f}u, over {} contacts beside it (the rest came to rest on a cap, "
                      "where there is no radius to measure)",
                      tested, settled, entered, worstGap, tapered);
            if (tapered > 0) {
                // Asserted for whatever shape the loader chose, not only for a
                // tapered one: a box around a square column passes this too,
                // and a box around a ROUND column - which is what this used to
                // be, at 0.92u - fails it. Gating the assertion on the kind
                // would make a regression to bounds skip the check rather than
                // trip it. What is left is the resolver's own skin plus the
                // flats of an octagon inside the circle drawn round it; the
                // load-time proxy line reports how round the ring is.
                ok &= expect(worstGap < 0.25f,
                             "a body rests within a quarter unit of the column's own surface");
            }
        }
    }

    // --- scaling a column scales its collision, and nothing else -----------
    //
    // The case this change exists for. An entity scaled in the editor keeps one
    // model and one proxy, both multiplied by the same number, so the gap
    // between what a body stops against and what the player can see is
    // multiplied too: the bounding box left 0.92u of air beside a column at
    // scale 1 and 2.8u at scale 3, which is most of a ship.
    //
    // So the thing to assert is not a distance, it is that the distance is
    // PROPORTIONATE - measured against the column's own surface at that height,
    // the standoff per unit of scale must not grow. A proxy that tracks the
    // model has a flat line here; the box had one that climbed.
    {
        int column = -1;
        for (size_t oi = 0; oi < level.objectProxies.size(); ++oi) {
            if (sim::proxyIsColumn(level.objectProxies[oi])) {
                column = static_cast<int>(oi);
                break;
            }
        }

        if (column < 0) {
            log::info("column scaling: SKIPPED, no column in this level");
        } else {
            const sim::PlacedProxy& placed = level.objectProxies[static_cast<size_t>(column)];
            // Back to an unplaced volume, so it can be put down again at other
            // scales through the same call the level loader uses.
            const sim::ProxyVolume volume =
                placed.kind == sim::ProxyVolume::Kind::TaperedCylinder
                    ? sim::taperedCylinderProxy(placed.radii.x, placed.radii.y,
                                                placed.halfExtents.y)
                    : sim::boxProxy(-placed.halfExtents, placed.halfExtents);

            constexpr float kSkin = 0.01f;
            const float radius = flight.shipRadius;
            float worstPerScale = 0.0f;
            float worstAt = 1.0f;
            int missed = 0;
            int checked = 0;

            for (const float scale : {1.0f, 2.0f, 4.0f, 7.5f}) {
                const sim::PlacedProxy grown =
                    sim::placeProxy(volume, placed.centre, placed.orientation, scale);
                const float half = grown.halfExtents.y;

                for (const glm::vec3& dir : dirs) {
                    // Sideways only: a cap has no radius to be measured against,
                    // and is the box's one good fit anyway.
                    const glm::vec3 flat(dir.x, 0.0f, dir.z);
                    if (glm::length(flat) < 1e-3f) continue;
                    const glm::vec3 out = glm::normalize(flat);

                    for (const float fraction : {-0.6f, -0.3f, 0.0f, 0.3f, 0.6f}) {
                        const float y = fraction * half;
                        const float surface = columnRadiusAt(grown, y);

                        // Fly it in, rather than starting it inside and pushing
                        // it out. The two are not the same question: a body
                        // buried in a box leaves through the nearest FACE
                        // whatever direction it came from, so pushing out never
                        // visits a corner, and a corner is exactly where the
                        // air was. Approaching is what the player does.
                        glm::vec3 position =
                            grown.centre + grown.orientation * glm::vec3(0.0f, y, 0.0f) +
                            out * (surface + radius + 4.0f * scale);

                        const float step = 0.02f * scale;
                        bool touched = false;
                        sim::ProxyContact contact;
                        for (int i = 0; i < 400; ++i) {
                            position -= out * step;
                            if (!sim::proxySphereContact(grown, position, radius, contact)) {
                                continue;
                            }
                            touched = true;
                            position += contact.normal * (contact.depth + kSkin);
                        }
                        if (!touched) {
                            ++missed;
                            continue;
                        }

                        ++checked;
                        const glm::vec3 local =
                            glm::conjugate(grown.orientation) * (position - grown.centre);
                        const float radial = glm::length(glm::vec2(local.x, local.z));
                        const float gap = radial - radius - columnRadiusAt(grown, local.y);
                        if (gap / scale > worstPerScale) {
                            worstPerScale = gap / scale;
                            worstAt = scale;
                        }
                    }
                }
            }

            ok &= expect(missed == 0, "a body flown at a scaled column always reaches it");
            // The push-out overshoots by one skin, and by the sliver between a
            // surface normal and the radial direction on a taper. Both are
            // fixed costs, so dividing by the scale is what makes this a real
            // bound rather than a number that drifts up with it.
            ok &= expect(worstPerScale < 0.05f,
                         "a scaled column stands a body off at its own scaled surface");
            log::info("column scaling: {} contacts over scales 1 to 7.5, worst standoff {:.3f}u "
                      "per unit of scale (at {:.1f}x); the bounding box this replaced left "
                      "0.91u per unit of scale at the corners",
                      checked, worstPerScale, worstAt);
        }
    }

    // --- bodies in different rooms do not shove each other through rock ----
    //
    // Segments share their vertices, so two rooms side by side are separated by
    // a wall of no thickness at all. Two bodies hugging that wall from opposite
    // sides are well inside each other's radii in world space while having no
    // business touching. The pair test is gated on the segment graph for this
    // reason, and the positive control below is what keeps the gate honest -
    // without it, a rule that rejected every pair would pass just as well.
    {
        // Two segments that are not neighbours.
        int farA = -1;
        int farB = -1;
        for (int i = 0; i < n && farA < 0; ++i) {
            for (int j = i + 1; j < n; ++j) {
                bool neighbours = false;
                for (const SideData& sd : level.segments[static_cast<size_t>(i)].sides) {
                    if (sd.neighbour == j) neighbours = true;
                }
                if (!neighbours) {
                    farA = i;
                    farB = j;
                    break;
                }
            }
        }

        if (farA < 0) {
            log::info("cross-room: SKIPPED, every segment neighbours every other");
        } else {
            // Coincident in world space - the worst possible overlap - but in
            // rooms the graph says do not meet.
            const glm::vec3 shared = level.segments[static_cast<size_t>(farA)].center;
            sim::Body a;
            sim::Body b;
            a.radius = b.radius = flight.shipRadius;
            a.mass = b.mass = 1.5f;
            a.position = b.position = shared;
            a.segment = farA;
            b.segment = farB;

            sim::BodySet set;
            set.add(a);
            set.add(b);
            const sim::BodyResolveStats stats =
                sim::resolveBodies(level, set, collision, kStep, nullptr, nullptr);

            const bool untouched = stats.bodyContacts == 0 && a.position == shared &&
                                   b.position == shared;
            ok &= expect(untouched, "bodies in non-adjacent rooms do not push each other");

            // Positive control: the same two bodies, same place, one room.
            sim::Body c;
            sim::Body d;
            c.radius = d.radius = flight.shipRadius;
            c.mass = d.mass = 1.5f;
            c.position = d.position = shared;
            c.segment = d.segment = farA;
            sim::BodySet together;
            together.add(c);
            together.add(d);
            const sim::BodyResolveStats controlStats =
                sim::resolveBodies(level, together, collision, kStep, nullptr, nullptr);
            const bool separated = controlStats.bodyContacts > 0 &&
                                   glm::length(d.position - c.position) > 1e-3f;
            ok &= expect(separated, "the same two bodies in ONE room do separate");

            log::info("cross-room: segments {} and {} share no portal - {} contacts; the same "
                      "pair in segment {} separated to {:.2f}u apart",
                      farA, farB, stats.bodyContacts, farA,
                      glm::length(d.position - c.position));
        }
    }

    // --- the same setup twice lands on the same numbers -------------------
    //
    // Sequential resolution is order dependent, which is fine as long as the
    // order is fixed. The fixed timestep is meant to be reproducible, and pairs
    // are walked by slot index precisely so that it stays that way.
    {
        const auto runOnce = [&](int bodies) {
            sim::Body b[4];
            sim::BodySet set;
            for (int i = 0; i < bodies; ++i) {
                b[i].radius = 1.6f + 0.1f * static_cast<float>(i);
                b[i].mass = 0.8f + 0.4f * static_cast<float>(i);
                b[i].segment = openSegment;
                b[i].position = openCentre + dirs[static_cast<size_t>(i)] * 2.0f;
                b[i].velocity = -dirs[static_cast<size_t>(i)] * 8.0f;
                set.add(b[i], 0.25f);
            }
            for (int step = 0; step < 60; ++step) {
                for (int i = 0; i < bodies; ++i) b[i].position += b[i].motion() * kStep;
                sim::resolveBodies(level, set, collision, kStep, nullptr, nullptr);
            }
            glm::vec3 sum(0.0f);
            for (int i = 0; i < bodies; ++i) sum += b[i].position + b[i].motion();
            return sum;
        };
        const glm::vec3 first = runOnce(4);
        const glm::vec3 second = runOnce(4);
        const bool identical = first == second;
        ok &= expect(identical, "the same body setup resolves to the same numbers twice");
        log::info("determinism: four bodies, 60 steps, two runs {}",
                  identical ? "bit-identical" : "DIVERGED");
    }

    // --- the tunnelling margin, reported rather than closed ----------------
    //
    // Body-against-body is a discrete overlap test, so in principle two bodies
    // can cross between steps. In practice they cannot, and this is the number
    // that says so. It is an assertion rather than a swept test because the
    // margin is an order of magnitude: paying for continuous collision to cover
    // a case the tuning cannot reach would be carrying cost forever.
    {
        // The smallest pair in the game: the ship and a drone.
        const float smallestPair = flight.shipRadius + 1.9f;
        // Two bodies at full speed, head on. Knockback can exceed cruising
        // speed, so double it again for headroom.
        const float closingSpeed = flight.maxSpeed * 2.0f * 2.0f;
        const float crossedPerStep = closingSpeed * kStep;
        const float margin = smallestPair / crossedPerStep;

        ok &= expect(margin > 4.0f,
                     "the smallest body pair cannot cross each other in one step");
        log::info("tunnelling margin: smallest pair {:.1f}u, worst closing {:.0f} u/s covers "
                  "{:.3f}u per step - {:.0f}x margin", smallestPair, closingSpeed, crossedPerStep,
                  margin);
    }

    log::info("collision test: {}", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace dvk
