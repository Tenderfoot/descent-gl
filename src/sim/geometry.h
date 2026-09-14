#pragma once

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>

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

// Moller-Trumbore. Returns the distance along a unit `dir`, or a negative
// number if the ray misses. Two-sided on purpose: a ray leaving the mine hits
// the back of a wall, and that still counts.
inline float rayTriangleDistance(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& a,
                                 const glm::vec3& b, const glm::vec3& c) {
    constexpr float kEpsilon = 1e-5f;
    const glm::vec3 ab = b - a;
    const glm::vec3 ac = c - a;
    const glm::vec3 pv = glm::cross(dir, ac);
    const float det = glm::dot(ab, pv);
    if (std::abs(det) < kEpsilon) return -1.0f;

    const float inv = 1.0f / det;
    const glm::vec3 tv = origin - a;
    const float u = glm::dot(tv, pv) * inv;
    if (u < 0.0f || u > 1.0f) return -1.0f;

    const glm::vec3 qv = glm::cross(tv, ab);
    const float v = glm::dot(dir, qv) * inv;
    if (v < 0.0f || u + v > 1.0f) return -1.0f;

    return glm::dot(ac, qv) * inv;
}

inline float distanceToTriangle(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b,
                                const glm::vec3& c) {
    return glm::length(p - closestPointOnTriangle(p, a, b, c));
}

} // namespace dvk::sim
