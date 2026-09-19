#pragma once

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

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

// The radius of a conical frustum at a height `y` in its own space, where the
// axis is Y, the radius is `radii.x` at `y = -halfHeight` and `radii.y` at
// `y = +halfHeight`. Both radii are positive in every frustum this project
// builds, so the radius never crosses zero inside the solid and the queries
// below do not have to ask which nappe of the cone they are on.
inline float taperedRadiusAt(const glm::vec2& radii, float halfHeight, float y) {
    if (halfHeight <= 0.0f) return radii.x;
    const float t = (y + halfHeight) / (2.0f * halfHeight);
    return radii.x + (radii.y - radii.x) * t;
}

// Closest point to p on a conical frustum centred on the origin with its axis
// along Y. A point inside comes back unchanged, so a caller can tell contact
// from penetration the same way it does for a box.
//
// A frustum is a solid of revolution, so this is a two-dimensional problem in
// the (radius, height) half plane, where the solid is the trapezoid with
// corners (0, -h), (rLow, -h), (rHigh, +h), (0, +h). The nearest point on that
// trapezoid, swung back around the axis, is the answer. Its three faces - the
// two caps and the slanted side - are tested outright rather than clamped,
// because a clamp is only correct for a box: the side is not axis aligned, so
// there is no per-axis interval to clamp into.
inline glm::vec3 closestPointOnTaperedCylinder(const glm::vec3& p, const glm::vec2& radii,
                                               float halfHeight) {
    const float h = halfHeight;
    const float d = std::sqrt(p.x * p.x + p.z * p.z);

    if (std::abs(p.y) <= h && d <= taperedRadiusAt(radii, h, p.y)) return p;

    // Candidate 1 and 2: straight onto a cap, which for a point off to the side
    // means the cap's rim.
    float bestD = glm::clamp(d, 0.0f, radii.x);
    float bestY = -h;
    float best = (d - bestD) * (d - bestD) + (p.y + h) * (p.y + h);

    const float topD = glm::clamp(d, 0.0f, radii.y);
    const float topGap = (d - topD) * (d - topD) + (p.y - h) * (p.y - h);
    if (topGap < best) {
        best = topGap;
        bestD = topD;
        bestY = h;
    }

    // Candidate 3: the slanted side, as a segment from (rLow, -h) to (rHigh, h).
    const glm::vec2 a(radii.x, -h);
    const glm::vec2 edge(radii.y - radii.x, 2.0f * h);
    const float lengthSquared = glm::dot(edge, edge);
    if (lengthSquared > 1e-12f) {
        const glm::vec2 toPoint(d - a.x, p.y - a.y);
        const float t = glm::clamp(glm::dot(toPoint, edge) / lengthSquared, 0.0f, 1.0f);
        const glm::vec2 on = a + edge * t;
        const float gap = (d - on.x) * (d - on.x) + (p.y - on.y) * (p.y - on.y);
        if (gap < best) {
            best = gap;
            bestD = on.x;
            bestY = on.y;
        }
    }

    // Back out of the half plane. A point on the axis has no radial direction,
    // and needs none: the nearest point there is on the axis too.
    if (d > 1e-6f) {
        const float spread = bestD / d;
        return glm::vec3(p.x * spread, bestY, p.z * spread);
    }
    return glm::vec3(bestD, bestY, 0.0f);
}

// Distance along a unit `dir` at which the ray enters a conical frustum centred
// on the origin with its axis along Y, or a negative number if it misses. An
// origin already inside returns 0. Same contract as `rayBoxDistance`.
//
// The slab clip on the caps is the box test's, and what is left is a quadratic:
// the ray is inside the cone where x^2 + z^2 <= radius(y)^2, and radius is
// linear in y, so squaring both sides gives one quadratic in t.
//
// Which stretch of the ray that quadratic marks as inside depends on its
// leading coefficient - between the roots for a ray crossing the column, but
// OUTSIDE them for a ray running up the taper steeply enough to stay within the
// cone's own half angle, and the whole span when there are no roots at all.
// Rather than a case for each, the span is cut at the roots and the sign is
// evaluated at the midpoint of each piece, which answers all three the same
// way. The frustum is convex and bounded, so a ray is inside it over exactly
// one stretch and the first such piece is the entry.
inline float rayTaperedCylinderDistance(const glm::vec3& origin, const glm::vec3& dir,
                                        const glm::vec2& radii, float halfHeight) {
    constexpr float kParallel = 1e-8f;
    const float h = halfHeight;
    if (h <= 0.0f) return -1.0f;

    // The window between the two caps.
    float lo = 0.0f;
    float hi = std::numeric_limits<float>::max();
    if (std::abs(dir.y) < kParallel) {
        if (origin.y < -h || origin.y > h) return -1.0f;
    } else {
        float t0 = (-h - origin.y) / dir.y;
        float t1 = (h - origin.y) / dir.y;
        if (t0 > t1) std::swap(t0, t1);
        lo = std::max(lo, t0);
        hi = std::min(hi, t1);
    }
    // The solid is bounded, so nothing past this can be on it. This is also
    // what keeps `hi` finite for a ray running parallel to the caps, and a
    // finite `hi` is what lets the midpoints below be evaluated at all.
    const float widest = std::max(radii.x, radii.y);
    hi = std::min(hi, glm::length(origin) + std::sqrt(widest * widest + h * h));
    if (hi < lo) return -1.0f;

    // radius(y) = a + b * y.
    const float b = (radii.y - radii.x) / (2.0f * h);
    const float a = 0.5f * (radii.x + radii.y);
    const float k = a + b * origin.y;
    const float m = b * dir.y;
    const float qa = dir.x * dir.x + dir.z * dir.z - m * m;
    const float qb = 2.0f * (origin.x * dir.x + origin.z * dir.z - k * m);
    const float qc = origin.x * origin.x + origin.z * origin.z - k * k;
    const auto inside = [&](float t) { return (qa * t + qb) * t + qc <= 0.0f; };

    float bounds[4];
    int count = 0;
    bounds[count++] = lo;
    if (std::abs(qa) > kParallel) {
        const float discriminant = qb * qb - 4.0f * qa * qc;
        if (discriminant >= 0.0f) {
            const float root = std::sqrt(discriminant);
            float r0 = (-qb - root) / (2.0f * qa);
            float r1 = (-qb + root) / (2.0f * qa);
            if (r0 > r1) std::swap(r0, r1);
            if (r0 > lo && r0 < hi) bounds[count++] = r0;
            if (r1 > lo && r1 < hi && r1 > r0) bounds[count++] = r1;
        }
    } else if (std::abs(qb) > kParallel) {
        const float root = -qc / qb;
        if (root > lo && root < hi) bounds[count++] = root;
    }
    bounds[count++] = hi;

    for (int i = 0; i + 1 < count; ++i) {
        if (inside(0.5f * (bounds[i] + bounds[i + 1]))) return std::max(0.0f, bounds[i]);
    }
    // A window of no width, where a midpoint says nothing: the ray grazes the
    // rim exactly, and the one point it touches is the answer or there is none.
    return inside(lo) ? std::max(0.0f, lo) : -1.0f;
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
