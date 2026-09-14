#pragma once

#include "sim/ship.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

namespace dvk {

// Which camera the renderer builds its view matrix from. First person is the
// game; chase exists so the ship model is visible while its flight constants
// are being tuned.
enum class CameraMode { FirstPerson, Chase };

// State advanced by the fixed 120 Hz step. Everything the renderer needs to
// interpolate lives here and nowhere else.
struct SimState {
    sim::ShipState ship;
};

// What the renderer draws: SimState interpolated between the two most recent
// steps. Keeping it a distinct type is what stops render code from quietly
// reading or mutating simulation state.
//
// The camera mode is set by main after interpolation rather than simulated: it
// is a view of the world, not part of it.
struct RenderState {
    glm::vec3 shipPos{0.0f};
    glm::quat shipOrientation{1.0f, 0.0f, 0.0f, 0.0f};
    CameraMode camera = CameraMode::FirstPerson;
    int shipSegment = 0;

    // Readouts, not used to draw the scene.
    float speed = 0.0f;         // units/s
    float angularSpeed = 0.0f;  // rad/s
};

inline RenderState interpolate(const SimState& a, const SimState& b, float t) {
    RenderState r;
    r.shipPos = glm::mix(a.ship.body.position, b.ship.body.position, t);
    // slerp, not mix: the shortest arc on the unit sphere. Component-wise
    // interpolation of a quaternion is only approximately a rotation, and the
    // error shows as a hitch at high turn rates.
    r.shipOrientation = glm::slerp(a.ship.orientation, b.ship.orientation, t);
    r.shipSegment = b.ship.body.segment;
    r.speed = glm::length(glm::mix(a.ship.body.velocity, b.ship.body.velocity, t));
    r.angularSpeed = glm::length(glm::mix(a.ship.angularVelocity(), b.ship.angularVelocity(), t));
    return r;
}

} // namespace dvk
