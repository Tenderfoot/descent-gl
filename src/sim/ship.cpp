#include "sim/ship.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace dvk::sim {

namespace {

// Exponential decay over dt. Expressed as a rate rather than a per-step factor
// so the feel does not change if the step rate ever does.
float decay(float rate, float dt) {
    return std::exp(-rate * dt);
}

// Clamps a vector's length without normalising a zero vector.
glm::vec3 clampLength(glm::vec3 v, float maxLength) {
    const float lenSq = glm::dot(v, v);
    if (lenSq > maxLength * maxLength && lenSq > 0.0f) {
        v *= maxLength / std::sqrt(lenSq);
    }
    return v;
}

} // namespace

void stepShip(ShipState& s, const ShipControls& c, const FlightConfig& cfg, float dt) {
    // --- angular ---------------------------------------------------------
    //
    // Mouse motion arrives as an angular-velocity impulse. Under exponential
    // damping an impulse of w produces a total rotation of w / angularDamping
    // no matter how it is spread over time, so a swipe turns the ship by the
    // same amount whether it landed in one step or ten. That is what makes the
    // mouse feel like a mouse instead of like a joystick.
    // Keyboard axes are torques: held down they ramp to a steady rate of
    // torque / angularDamping. This is the channel maxAngularSpeed governs.
    s.thrusterSpin.x += c.pitch * cfg.pitchTorque * dt;
    s.thrusterSpin.y += c.yaw * cfg.yawTorque * dt;
    s.thrusterSpin.z += -c.roll * cfg.rollTorque * dt;
    s.thrusterSpin = clampLength(s.thrusterSpin, cfg.maxAngularSpeed);
    s.thrusterSpin *= decay(cfg.angularDamping, dt);

    // Mouse motion is an angular-velocity impulse on the uncapped channel, so
    // the turn carries the ship's rotational inertia and settles instead of
    // snapping. Damping is what consumes the impulse, which makes the total
    // rotation impulse / angularDamping however the motion was spread across
    // steps: a swipe turns the ship by the same amount whether it arrived in
    // one frame or in thirty.
    const float pitchSign = cfg.invertPitch ? 1.0f : -1.0f;
    s.aimSpin.x += pitchSign * c.look.y * cfg.mouseSensitivity;
    s.aimSpin.y += -c.look.x * cfg.mouseSensitivity;
    s.aimSpin *= decay(cfg.angularDamping, dt);

    const glm::vec3 angularVelocity = s.angularVelocity();

    // Integrate the quaternion. The angular velocity is in the local frame, so
    // it multiplies on the right: dq/dt = 0.5 * q * (0, w).
    const glm::quat spin =
        s.orientation * glm::quat(0.0f, angularVelocity * 0.5f * dt);
    s.orientation = glm::normalize(s.orientation + spin);

    // --- linear ----------------------------------------------------------
    //
    // Thrust is local, velocity is world, so the acceleration is rotated once
    // here and momentum then carries on in world space. Letting go of the stick
    // leaves the ship drifting the way it was already going, which is the whole
    // feel of the game.
    const glm::vec3 localAccel{
        c.strafe * cfg.strafeThrust,
        c.lift * cfg.liftThrust,
        -c.thrust * cfg.forwardThrust,   // forward is -Z
    };
    s.body.velocity += (s.orientation * localAccel) * dt;

    s.body.velocity *= decay(cfg.linearDamping, dt);
    s.body.velocity = clampLength(s.body.velocity, cfg.maxSpeed);

    // The knockback channel sits outside both, deliberately. linearDamping and
    // maxSpeed describe the ship's engines and what its hull will hold; a blast
    // is neither, and running it through them would clip every shove down to
    // cruising speed and then bleed it away in a third of a second. See
    // Body::knockback.
    s.body.knockback *= decay(kKnockbackDamping, dt);

    s.body.position += s.body.motion() * dt;
}

} // namespace dvk::sim
