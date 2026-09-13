#pragma once

#include <glm/mat4x4.hpp>

#include <cmath>

namespace dvk::gl {

// Reversed-Z projection with an infinite far plane.
//
// Depth runs near = 1.0 -> infinity = 0.0, paired with glClipControl(ZERO_TO_ONE),
// a clear depth of 0.0 and GL_GEQUAL. Two reasons, both structural:
//
//  * Float precision clusters near 0. Reversed-Z puts the far distances there,
//    which is where depth fighting would otherwise show.
//  * M3 extrudes shadow volumes to infinity by emitting w = 0. With a finite
//    far plane those vertices clip; with this projection they land exactly on
//    the far plane and the stencil counts stay correct.
//
// Changing this affects every depth comparison and the shadow volumes. Do not
// swap it for a conventional projection without revisiting M3.
inline glm::mat4 infiniteReversedZ(float fovYRadians, float aspect, float zNear) {
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);
    glm::mat4 m(0.0f);
    m[0][0] = f / aspect;
    m[1][1] = f;
    m[2][3] = -1.0f;
    m[3][2] = zNear;
    return m;
}

} // namespace dvk::gl
