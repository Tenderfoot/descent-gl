#pragma once

#include "sim/flight.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace dvk::sim {

// What the player is asking the ship to do this step, normalised and free of
// any input-device detail. game/bindings.h fills this in from the keyboard and
// mouse; the simulation never sees a GLFW key code.
struct ShipControls {
    float thrust = 0.0f;   // -1 back .. +1 forward
    float strafe = 0.0f;   // -1 left .. +1 right
    float lift = 0.0f;     // -1 down .. +1 up
    float roll = 0.0f;     // -1 left .. +1 right
    float pitch = 0.0f;    // keyboard pitch, -1 down .. +1 up
    float yaw = 0.0f;      // keyboard yaw, -1 left .. +1 right
    glm::vec2 look{0.0f};  // mouse motion in pixels, x = yaw, y = pitch
};

// The ship, integrated by the fixed 120 Hz step.
//
// Orientation is a quaternion and only ever a quaternion. Euler angles are
// never stored and never accumulated, which is what keeps a 6DOF ship from
// gimbal-locking when it flies straight up a shaft.
//
// Angular velocity is in the ship's LOCAL frame (x = pitch, y = yaw, z = roll).
// Linear velocity is in WORLD space. Keeping them in different frames is
// deliberate: thrust is naturally local, momentum is naturally global.
//
// Rotation is carried in two channels that are summed to get the real angular
// velocity, because the two have genuinely different limits:
//
//   thrusterSpin  what the manoeuvring thrusters produce, from the keyboard
//                 axes. Capped by maxAngularSpeed - the ship can only push
//                 itself around so fast.
//   aimSpin       what the mouse asks for. NOT capped. A player flicking
//                 across the pad expects the whole turn; clipping it makes a
//                 fast swipe cover less ground than a slow one of the same
//                 length, which is what makes mouse look feel broken.
//
// One clamped channel cannot express both: the cap would catch the leftover
// aim velocity on the step after the swipe and eat most of the turn.
struct ShipState {
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 velocity{0.0f};       // world, units/s
    glm::vec3 thrusterSpin{0.0f};   // local, rad/s, capped
    glm::vec3 aimSpin{0.0f};        // local, rad/s, uncapped

    glm::vec3 angularVelocity() const { return thrusterSpin + aimSpin; }
};

// Advances one fixed step. dt is always kSimStep; it is a parameter only so the
// integration reads honestly.
void stepShip(ShipState& s, const ShipControls& c, const FlightConfig& cfg, float dt);

// Local axes, derived from the orientation. Forward is -Z, matching GL's
// convention and the projection in render/camera.h.
inline glm::vec3 shipForward(const ShipState& s) {
    return s.orientation * glm::vec3(0.0f, 0.0f, -1.0f);
}
inline glm::vec3 shipUp(const ShipState& s) {
    return s.orientation * glm::vec3(0.0f, 1.0f, 0.0f);
}
inline glm::vec3 shipRight(const ShipState& s) {
    return s.orientation * glm::vec3(1.0f, 0.0f, 0.0f);
}

} // namespace dvk::sim
