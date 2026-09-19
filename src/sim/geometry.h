#pragma once

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace dvk::sim {

// Small geometric primitives shared by lighting, visibility and collision.
// Header-only and allocation-free: these run per triangle in the inner loops
// of the sim step and the light culling.

// Closest point to p on the triangle abc, handling the face, the three edges
// and the three vertices.
//
// Ericson, Real-Time Collision Detection, section 5.1.5: the barycentric
// region test. Written out rather than reduced because the edge and vertex
// cases are exactly the ones a naive plane projection gets wrong, and M4's
// collision falls through walls when it does - a sphere resting on a
// triangle's edge must not be pushed through it.
inline glm::vec3 closestPointOnTriangle(const glm::vec3& p, const glm::vec3& a,
                                        const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 ab = b - a;
    const glm::vec3 ac = c - a;
    const glm::vec3 ap = p - a;

    const float d1 = glm::dot(ab, ap);
    const float d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;   // vertex region A

    const glm::vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp);
    const float d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;     // vertex region B

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float denom = d1 - d3;
        return a + ab * (denom != 0.0f ? d1 / denom : 0.0f);   // edge AB
    }

    const glm::vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp);
    const float d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;     // vertex region C

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float denom = d2 - d6;
        return a + ac * (denom != 0.0f ? d2 / denom : 0.0f);   // edge AC
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float denom = (d4 - d3) + (d5 - d6);
        return b + (c - b) * (denom != 0.0f ? (d4 - d3) / denom : 0.0f);   // edge BC
    }

    const float denom = va + vb + vc;
    if (denom == 0.0f) return a;   // degenerate triangle
    const float v = vb / denom;
    const float w = vc / denom;
    return a + ab * v + ac * w;    // face region
}

// Closest point to p on an axis-aligned box centred on the origin with the
// given half extents. The oriented case rotates p into the box's own frame,
// calls this, and rotates the answer back.
//
// One clamp, where the triangle above needs seven regions. That is the whole
// argument for giving a column a box rather than its own 28 triangles: a box
// is convex in its own space, so there is no edge case to get wrong and no
// branch to get wrong it in.
inline glm::vec3 closestPointOnBox(const glm::vec3& p, const glm::vec3& halfExtents) {
    return glm::clamp(p, -halfExtents, halfExtents);
}

// Distance along a unit `dir` at which the ray enters an axis-aligned box
// centred on the origin, or a negative number if it misses. An origin already
// inside the box returns 0.
//
// The slab test, written with an explicit guard for a direction component of
// zero rather than leaning on infinities. A ray exactly parallel to a slab
// produces 0 * inf = NaN in the reciprocal form, and a NaN loses every
// comparison it takes part in, so the miss is reported as a hit at whatever
// distance the other two axes happened to leave in the accumulator. Two extra
// lines here, instead of a bolt that stops in mid-air beside a pillar.
inline float rayBoxDistance(const glm::vec3& origin, const glm::vec3& dir,
                            const glm::vec3& halfExtents) {
    constexpr float kParallel = 1e-8f;
    // Not `near` and `far`: those are macros in <windef.h>, and the shipping
    // build is MSVC.
    float tNear = 0.0f;
    float tFar = std::numeric_limits<float>::max();

    for (int axis = 0; axis < 3; ++axis) {
        const float d = dir[axis];
        const float o = origin[axis];
        const float h = halfExtents[axis];
        if (std::abs(d) < kParallel) {
            // Parallel to this pair of planes: either always between them or
            // never.
            if (o < -h || o > h) return -1.0f;
            continue;
        }
        const float inv = 1.0f / d;
        float t0 = (-h - o) * inv;
        float t1 = (h - o) * inv;
        if (t0 > t1) std::swap(t0, t1);
        tNear = std::max(tNear, t0);
        tFar = std::min(tFar, t1);
        if (tNear > tFar) return -1.0f;
    }
    return tFar < 0.0f ? -1.0f : tNear;
}

// A convex polyhedron handed to the simulation: the planes that bound it, its
// corners, and each face's boundary. The arrays are owned elsewhere and outlive
// the view - `Level` builds them once per model type at load.
//
// `sim` still reads no asset. This is the same seam `ProxyVolume` established,
// widened from a value to a buffer: the game layer is the only thing that sees
// both an OBJ and a collider, and it hands finished numbers across.
struct ConvexHullView {
    const glm::vec4* planes = nullptr;     // xyz outward unit normal, w offset
    const glm::vec3* vertices = nullptr;
    const int* faceFirst = nullptr;        // planeCount + 1 entries
    const int* faceVertex = nullptr;       // indices into `vertices`
    int planeCount = 0;
    int vertexCount = 0;

    bool valid() const {
        return planes && vertices && faceFirst && faceVertex && planeCount >= 4;
    }
    // Inside is `dot(n, x) + d <= 0` on every plane.
    float signedDistance(int plane, const glm::vec3& p) const {
        return glm::dot(glm::vec3(planes[plane]), p) + planes[plane].w;
    }
};

// Distance along a unit `dir` at which the ray enters a convex polyhedron, or a
// negative number if it misses. An origin already inside returns 0. Same
// contract as `rayBoxDistance`, and the same algorithm: a box IS this with six
// planes, so the only change is that the axes are no longer implied.
//
// `inflate` moves every face outward by that much, which for a polyhedron
// over-covers the edges and corners where the true swept shape is rounded. The
// same bias the box takes, for the same reason - a shot stops at something the
// player can see rather than visibly clipping through it.
inline float rayConvexHullDistance(const ConvexHullView& hull, const glm::vec3& origin,
                                   const glm::vec3& dir, float inflate = 0.0f) {
    constexpr float kParallel = 1e-8f;
    float tNear = 0.0f;
    float tFar = std::numeric_limits<float>::max();

    for (int i = 0; i < hull.planeCount; ++i) {
        const glm::vec3 n(hull.planes[i]);
        const float offset = hull.planes[i].w - inflate;
        const float denom = glm::dot(n, dir);
        const float distance = glm::dot(n, origin) + offset;
        if (std::abs(denom) < kParallel) {
            // Parallel to this face: either always on its inner side or never.
            // Guarded explicitly rather than by dividing, for the reason
            // `rayBoxDistance` spells out.
            if (distance > 0.0f) return -1.0f;
            continue;
        }
        const float t = -distance / denom;
        if (denom < 0.0f) tNear = std::max(tNear, t);   // coming in through it
        else tFar = std::min(tFar, t);                  // going out through it
        if (tNear > tFar) return -1.0f;
    }
    return tNear;
}

// Closest point to `p` on a convex polyhedron. A point inside comes back
// unchanged, so a caller tells contact from penetration the way it does for a
// box.
//
// Every face is tested rather than only the ones `p` is outside of. That is the
// conservative reading: for a point beyond an edge the nearest point belongs to
// two faces at once, and which of them the arithmetic calls "violated" comes
// down to the last bit when the dihedral is shallow. A shipped hull has ten
// faces and the broad phase has already thrown out everything not nearby, so
// the saving was never worth the case analysis.
inline glm::vec3 closestPointOnConvexHull(const ConvexHullView& hull, const glm::vec3& p) {
    bool outside = false;
    for (int i = 0; i < hull.planeCount && !outside; ++i) {
        if (hull.signedDistance(i, p) > 0.0f) outside = true;
    }
    if (!outside) return p;

    glm::vec3 best(0.0f);
    float bestGap = std::numeric_limits<float>::max();

    for (int i = 0; i < hull.planeCount; ++i) {
        const glm::vec3 n(hull.planes[i]);
        const int first = hull.faceFirst[i];
        const int count = hull.faceFirst[i + 1] - first;
        if (count < 3) continue;

        // Straight down onto the face's plane, then ask whether that landed on
        // the face. The boundary is wound counter-clockwise about the outward
        // normal, so a point inside is to the left of every edge.
        const glm::vec3 onPlane = p - n * hull.signedDistance(i, p);
        bool within = true;
        for (int e = 0; e < count && within; ++e) {
            const glm::vec3& a = hull.vertices[hull.faceVertex[first + e]];
            const glm::vec3& b = hull.vertices[hull.faceVertex[first + (e + 1) % count]];
            if (glm::dot(glm::cross(b - a, onPlane - a), n) < 0.0f) within = false;
        }

        glm::vec3 candidate = onPlane;
        if (!within) {
            // Past the face's rim, so the nearest point on THIS face is on one
            // of its edges. Clamping to each in turn also covers the corners,
            // which is the case a plane projection alone gets wrong.
            float rim = std::numeric_limits<float>::max();
            for (int e = 0; e < count; ++e) {
                const glm::vec3& a = hull.vertices[hull.faceVertex[first + e]];
                const glm::vec3& b = hull.vertices[hull.faceVertex[first + (e + 1) % count]];
                const glm::vec3 along = b - a;
                const float lengthSquared = glm::dot(along, along);
                const float t = lengthSquared > 1e-12f
                                    ? glm::clamp(glm::dot(p - a, along) / lengthSquared, 0.0f, 1.0f)
                                    : 0.0f;
                const glm::vec3 on = a + along * t;
                const glm::vec3 away = p - on;
                const float gap = glm::dot(away, away);
                if (gap < rim) {
                    rim = gap;
                    candidate = on;
                }
            }
        }

        const glm::vec3 away = p - candidate;
        const float gap = glm::dot(away, away);
        if (gap < bestGap) {
            bestGap = gap;
            best = candidate;
        }
    }

    return bestGap < std::numeric_limits<float>::max() ? best : p;
}

// For a point INSIDE a convex polyhedron: how far it is from the nearest face,
// and that face's outward normal. Exact, and one pass - the nearest face to an
// interior point of a convex solid is simply the least of its plane distances,
// where a box needs the same rule and a triangle mesh has no such shortcut.
inline float convexHullEscape(const ConvexHullView& hull, const glm::vec3& p, glm::vec3& normal) {
    float least = std::numeric_limits<float>::max();
    for (int i = 0; i < hull.planeCount; ++i) {
        const float room = -hull.signedDistance(i, p);   // positive inside
        if (room < least) {
            least = room;
            normal = glm::vec3(hull.planes[i]);
        }
    }
    return least;
}

// Moller-Trumbore. Returns the distance along a unit `dir`, or a negative
// number if the ray misses. Two-sided on purpose: a ray leaving the mine hits
// the back of a wall, and that still counts.
// `edgeSlack` widens the barycentric bounds, so a ray passing exactly along a
// triangle's edge still counts as a hit. Zero for surfaces, where a hair of
// over-reporting would stop a ray short; a small positive value for portals,
// where the two triangles of one quad share a diagonal and a ray straight down
// that diagonal can otherwise miss both and be stopped by the wall behind.
inline float rayTriangleDistance(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& a,
                                 const glm::vec3& b, const glm::vec3& c, float edgeSlack = 0.0f) {
    constexpr float kEpsilon = 1e-5f;
    const glm::vec3 ab = b - a;
    const glm::vec3 ac = c - a;
    const glm::vec3 pv = glm::cross(dir, ac);
    const float det = glm::dot(ab, pv);
    if (std::abs(det) < kEpsilon) return -1.0f;

    const float inv = 1.0f / det;
    const glm::vec3 tv = origin - a;
    const float u = glm::dot(tv, pv) * inv;
    if (u < -edgeSlack || u > 1.0f + edgeSlack) return -1.0f;

    const glm::vec3 qv = glm::cross(tv, ab);
    const float v = glm::dot(dir, qv) * inv;
    if (v < -edgeSlack || u + v > 1.0f + edgeSlack) return -1.0f;

    return glm::dot(ac, qv) * inv;
}

inline float distanceToTriangle(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b,
                                const glm::vec3& c) {
    return glm::length(p - closestPointOnTriangle(p, a, b, c));
}

} // namespace dvk::sim
