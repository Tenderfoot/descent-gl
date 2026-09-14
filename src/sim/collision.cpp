#include "sim/collision.h"

#include "game/level.h"
#include "sim/geometry.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace dvk::sim {

namespace {

// Iteration caps. A mine is small; these only stop a degenerate level from
// spinning forever.
constexpr int kMaxSegmentSteps = 32;
constexpr int kMaxSweepSteps = 64;
constexpr float kEpsilon = 1e-5f;

// The two triangles a portal side is split into, so a ray can be tested against
// the opening itself rather than against the segment's bounding planes.
void portalTriangles(const Level& level, int segment, int side, glm::vec3 out[6]) {
    const std::array<glm::vec3, 4> c = level.sideCorners(segment, side);
    const SideData& sd = level.segments[static_cast<size_t>(segment)].sides[static_cast<size_t>(side)];
    if (sd.altDiagonal) {
        out[0] = c[0]; out[1] = c[1]; out[2] = c[3];
        out[3] = c[1]; out[4] = c[2]; out[5] = c[3];
    } else {
        out[0] = c[0]; out[1] = c[1]; out[2] = c[2];
        out[3] = c[0]; out[4] = c[2]; out[5] = c[3];
    }
}

} // namespace

bool doorShut(const SolidDoors* doors, int segment, int neighbour) {
    if (!doors) return false;
    for (int i = 0; i < doors->count; ++i) {
        const SolidDoors::Face& f = doors->faces[static_cast<size_t>(i)];
        if ((f.segmentA == segment && f.segmentB == neighbour) ||
            (f.segmentB == segment && f.segmentA == neighbour)) {
            return true;
        }
    }
    return false;
}

int resolveCollision(const Level& level, Body& body, const CollisionConfig& cfg, float dt,
                     ContactLog* log, const SolidDoors* doors) {
    if (log) log->clear();
    if (level.segments.empty()) return 0;

    int total = 0;

    for (int iteration = 0; iteration < cfg.iterations; ++iteration) {
        // Where the body is now, which the previous iteration may have changed.
        body.segment = trackSegment(level, body.position, body.segment);
        if (log) log->iterations = iteration + 1;

        int resolvedThisPass = 0;

        // The body's own segment, plus every segment on the far side of one of
        // its portals. A body that has just crossed a portal mid-step is up
        // against the neighbour's walls, not its own, and a body wider than a
        // portal must be stopped by the rim rather than squeezed through it.
        const Segment& seg = level.segments[static_cast<size_t>(body.segment)];
        int candidates[1 + SideCount];
        int candidateCount = 0;
        candidates[candidateCount++] = body.segment;
        for (const SideData& sd : seg.sides) {
            if (sd.isPortal()) candidates[candidateCount++] = sd.neighbour;
        }

        for (int ci = 0; ci < candidateCount; ++ci) {
            const int si = candidates[ci];
            const int first = level.wallFirst[static_cast<size_t>(si)];
            const int count = level.wallCount[static_cast<size_t>(si)];

            for (int wi = first; wi < first + count; ++wi) {
                const WallTriangle& w = level.walls[static_cast<size_t>(wi)];

                const glm::vec3 closest = closestPointOnTriangle(body.position, w.p[0], w.p[1],
                                                                 w.p[2]);
                glm::vec3 away = body.position - closest;
                const float distance = glm::length(away);
                if (distance >= body.radius) continue;

                // The push direction is away from the CLOSEST POINT, not along
                // the triangle's normal. On a face they agree; on an edge or a
                // corner they do not, and using the normal there pushes the
                // body sideways along the wall instead of off it. That is the
                // case where a sphere resting on a triangle's edge falls
                // through.
                glm::vec3 normal;
                if (distance > kEpsilon) {
                    normal = away / distance;
                    // If the body has already ended up behind the wall, the
                    // direction away from the closest point points further out
                    // of the mine. Fall back to the wall's own inward normal.
                    if (glm::dot(normal, w.normal) < 0.0f) normal = w.normal;
                } else {
                    normal = w.normal;
                }

                const float depth = body.radius - distance;
                body.position += normal * (depth + cfg.skin);

                // Remove only the part of the velocity going into the wall. The
                // tangential part survives, which is what makes the ship slide
                // along a wall instead of stopping dead on it.
                const float into = glm::dot(body.velocity, normal);
                if (into < 0.0f) body.velocity -= normal * into;

                if (log) {
                    Contact c;
                    c.point = closest;
                    c.normal = normal;
                    c.segment = si;
                    c.triangle = wi;
                    c.depth = depth;
                    log->add(c);
                }
                ++resolvedThisPass;
                ++total;
            }
        }

        // Shut doors, which are not in the static wall list because the side
        // they fill is a portal. Pushed out of on whichever side the body is,
        // so a door closing on a body shoves it clear rather than through.
        if (doors) {
            for (int fi = 0; fi < doors->count; ++fi) {
                const SolidDoors::Face& face = doors->faces[static_cast<size_t>(fi)];
                if (face.segmentA != body.segment && face.segmentB != body.segment) continue;

                const glm::vec3 closest =
                    closestPointOnTriangle(body.position, face.p[0], face.p[1], face.p[2]);
                const glm::vec3 away = body.position - closest;
                const float distance = glm::length(away);
                if (distance >= body.radius) continue;

                const glm::vec3 normal =
                    distance > kEpsilon
                        ? away / distance
                        : (glm::dot(body.velocity, face.normal) < 0.0f ? face.normal
                                                                       : -face.normal);
                body.position += normal * (body.radius - distance + cfg.skin);
                const float into = glm::dot(body.velocity, normal);
                if (into < 0.0f) body.velocity -= normal * into;

                if (log) {
                    Contact c;
                    c.point = closest;
                    c.normal = normal;
                    c.segment = body.segment;
                    c.triangle = -1;
                    c.depth = body.radius - distance;
                    log->add(c);
                }
                ++resolvedThisPass;
                ++total;
            }
        }

        if (resolvedThisPass == 0) break;
    }

    if (total > 0 && cfg.scrapeDamping > 0.0f) {
        body.velocity *= std::exp(-cfg.scrapeDamping * dt);
    }

    body.segment = trackSegment(level, body.position, body.segment);
    return total;
}

RayHit raycast(const Level& level, const glm::vec3& origin, const glm::vec3& direction,
               float maxDistance, int startSegment, const SolidDoors* doors) {
    RayHit result;
    if (level.segments.empty()) return result;

    const float length = glm::length(direction);
    if (length < kEpsilon) return result;
    const glm::vec3 dir = direction / length;

    int segment = startSegment >= 0 && startSegment < level.segmentCount()
                      ? startSegment
                      : findSegment(level, origin);
    if (segment < 0) return result;

    float travelled = 0.0f;

    for (int step = 0; step < kMaxSegmentSteps; ++step) {
        const int first = level.wallFirst[static_cast<size_t>(segment)];
        const int count = level.wallCount[static_cast<size_t>(segment)];

        float bestWall = maxDistance - travelled;
        int bestTriangle = -1;
        for (int wi = first; wi < first + count; ++wi) {
            const WallTriangle& w = level.walls[static_cast<size_t>(wi)];
            const float t = rayTriangleDistance(origin + dir * travelled, dir, w.p[0], w.p[1], w.p[2]);
            if (t > kEpsilon && t < bestWall) {
                bestWall = t;
                bestTriangle = wi;
            }
        }

        // The nearest portal the ray leaves through, if it leaves before it
        // meets a wall.
        // A shut door stops a ray exactly as a wall does, which is what keeps a
        // robot from seeing through one and a bolt from flying through it.
        if (doors) {
            for (int fi = 0; fi < doors->count; ++fi) {
                const SolidDoors::Face& face = doors->faces[static_cast<size_t>(fi)];
                if (face.segmentA != segment && face.segmentB != segment) continue;
                const float t = rayTriangleDistance(origin + dir * travelled, dir, face.p[0],
                                                    face.p[1], face.p[2]);
                if (t > kEpsilon && t < bestWall) {
                    bestWall = t;
                    bestTriangle = -2;   // a door, not a wall triangle
                }
            }
        }

        float bestPortal = bestWall;
        int nextSegment = -1;
        const Segment& seg = level.segments[static_cast<size_t>(segment)];
        for (int side = 0; side < SideCount; ++side) {
            const SideData& sd = seg.sides[static_cast<size_t>(side)];
            if (!sd.isPortal()) continue;
            if (doorShut(doors, segment, sd.neighbour)) continue;
            glm::vec3 tri[6];
            portalTriangles(level, segment, side, tri);
            for (int t2 = 0; t2 < 2; ++t2) {
                const float t = rayTriangleDistance(origin + dir * travelled, dir, tri[t2 * 3 + 0],
                                                    tri[t2 * 3 + 1], tri[t2 * 3 + 2]);
                if (t > kEpsilon && t < bestPortal) {
                    bestPortal = t;
                    nextSegment = sd.neighbour;
                }
            }
        }

        if (nextSegment >= 0) {
            // Nudged past the portal plane so the next segment's test does not
            // immediately find the same crossing.
            travelled += bestPortal + kEpsilon * 10.0f;
            if (travelled >= maxDistance) return result;
            segment = nextSegment;
            continue;
        }

        if (bestTriangle == -2) {
            result.hit = true;
            result.distance = travelled + bestWall;
            result.point = origin + dir * result.distance;
            result.normal = -dir;
            result.segment = segment;
            result.triangle = -1;
        } else if (bestTriangle >= 0) {
            const WallTriangle& w = level.walls[static_cast<size_t>(bestTriangle)];
            result.hit = true;
            result.distance = travelled + bestWall;
            result.point = origin + dir * result.distance;
            result.normal = w.normal;
            result.segment = segment;
            result.triangle = bestTriangle;
        }
        return result;
    }
    return result;
}

// Conservative advancement: step forward by the distance to the nearest wall,
// which can never step past it.
//
// The analytic swept-sphere-versus-triangle test is faster in the worst case
// but has separate face, edge and vertex branches, and it is exactly those
// branches that a projectile system finds the bugs in. This form is a handful
// of lines, cannot tunnel by construction, and is deterministic. Its weakness
// is grazing contact, where it converges slowly and gives up after
// kMaxSweepSteps and reports no hit - which for a projectile means it keeps
// flying, the safe answer.
RayHit sweepSphere(const Level& level, const glm::vec3& origin, const glm::vec3& direction,
                   float radius, float maxDistance, int startSegment, const SolidDoors* doors) {
    RayHit result;
    if (level.segments.empty()) return result;

    const float length = glm::length(direction);
    if (length < kEpsilon) return result;
    const glm::vec3 dir = direction / length;

    int segment = startSegment >= 0 && startSegment < level.segmentCount()
                      ? startSegment
                      : findSegment(level, origin);
    if (segment < 0) return result;

    float travelled = 0.0f;

    for (int step = 0; step < kMaxSweepSteps; ++step) {
        const glm::vec3 p = origin + dir * travelled;
        segment = trackSegment(level, p, segment);

        // The current segment and everything through one of its portals, so the
        // step length is a true lower bound on the distance to any wall the
        // sphere could reach before the next iteration re-centres it.
        const Segment& seg = level.segments[static_cast<size_t>(segment)];
        int candidates[1 + SideCount];
        int candidateCount = 0;
        candidates[candidateCount++] = segment;
        for (const SideData& sd : seg.sides) {
            if (sd.isPortal() && !doorShut(doors, segment, sd.neighbour)) {
                candidates[candidateCount++] = sd.neighbour;
            }
        }

        float nearest = maxDistance * 2.0f + radius;
        int nearestTriangle = -1;
        int nearestSegment = segment;
        for (int ci = 0; ci < candidateCount; ++ci) {
            const int si = candidates[ci];
            const int first = level.wallFirst[static_cast<size_t>(si)];
            const int count = level.wallCount[static_cast<size_t>(si)];
            for (int wi = first; wi < first + count; ++wi) {
                const WallTriangle& w = level.walls[static_cast<size_t>(wi)];
                const float d = distanceToTriangle(p, w.p[0], w.p[1], w.p[2]);
                if (d < nearest) {
                    nearest = d;
                    nearestTriangle = wi;
                    nearestSegment = si;
                }
            }
        }

        // Shut doors are surfaces too, and a bolt has to stop at one.
        bool nearestIsDoor = false;
        if (doors) {
            for (int fi = 0; fi < doors->count; ++fi) {
                const SolidDoors::Face& face = doors->faces[static_cast<size_t>(fi)];
                if (face.segmentA != segment && face.segmentB != segment) continue;
                const float d = distanceToTriangle(p, face.p[0], face.p[1], face.p[2]);
                if (d < nearest) {
                    nearest = d;
                    nearestIsDoor = true;
                    nearestSegment = segment;
                }
            }
        }

        const float gap = nearest - radius;
        if (gap <= kEpsilon * 10.0f) {
            if (nearestIsDoor) {
                result.hit = true;
                result.distance = travelled;
                result.point = p;
                result.normal = -dir;
                result.segment = nearestSegment;
                result.triangle = -1;
                return result;
            }
            if (nearestTriangle < 0) return result;
            const WallTriangle& w = level.walls[static_cast<size_t>(nearestTriangle)];
            result.hit = true;
            result.distance = travelled;
            result.point = closestPointOnTriangle(p, w.p[0], w.p[1], w.p[2]);
            result.normal = w.normal;
            result.segment = nearestSegment;
            result.triangle = nearestTriangle;
            return result;
        }

        travelled += gap;
        if (travelled >= maxDistance) return result;
    }
    return result;
}

} // namespace dvk::sim
