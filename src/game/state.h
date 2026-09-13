#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

namespace dvk {

// State advanced by the fixed 120 Hz step. Everything the renderer needs to
// interpolate lives here and nowhere else.
struct SimState {
    float cubeAngle = 0.0f;          // radians
    glm::vec3 camPos{0.0f, 0.0f, 3.5f};
    float camYaw = 0.0f;             // radians, +Y axis
    float camPitch = 0.0f;           // radians, clamped to +/- 85 degrees
};

// What the renderer draws: SimState interpolated between the two most recent
// steps. Keeping it a distinct type is what stops render code from quietly
// reading or mutating simulation state.
struct RenderState {
    float cubeAngle = 0.0f;
    glm::vec3 camPos{0.0f};
    float camYaw = 0.0f;
    float camPitch = 0.0f;
};

inline RenderState interpolate(const SimState& a, const SimState& b, float t) {
    RenderState r;
    r.cubeAngle = glm::mix(a.cubeAngle, b.cubeAngle, t);
    r.camPos = glm::mix(a.camPos, b.camPos, t);
    r.camYaw = glm::mix(a.camYaw, b.camYaw, t);
    r.camPitch = glm::mix(a.camPitch, b.camPitch, t);
    return r;
}

} // namespace dvk
