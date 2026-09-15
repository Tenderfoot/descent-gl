#pragma once

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

namespace dvk::sim {

// Anything that moves through the mine and is stopped by its walls. The ship is
// one; robots, projectiles and debris become Bodies in later milestones.
//
// Collision is sphere-based and only sphere-based. A 6DOF shooter's ships and
// robots are compact and the mine is made of large flat faces, so an oriented
// box would cost more in cases to get wrong than it buys in fit. One radius
// also keeps the resolve loop short enough to run four times a step.
struct Body {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};   // world, units/s
    // Velocity the body did not ask for: a blast wave, a bullet's punch.
    //
    // A separate channel because everything that owns `velocity` would erase
    // it. The flight model damps velocity toward zero and then clamps it to
    // maxSpeed, so a shove faster than cruising speed would be gone in the step
    // it arrived in; a robot's steering drives velocity toward whatever it
    // means to be doing. Neither gets a say in this one.
    //
    // Walls stop it exactly as they stop `velocity` - resolveCollision takes
    // the inward part out of both - so being blown into a wall is not a way
    // through it.
    glm::vec3 knockback{0.0f};
    float radius = 1.6f;
    // Which segment the body is in. Tracked by following portal planes rather
    // than searched for, so it costs O(portals crossed) and not O(segments).
    int segment = 0;

    // What the body actually moves at. Every integration site uses this; none
    // of them integrate `velocity` alone.
    glm::vec3 motion() const { return velocity + knockback; }
};

// How fast the knockback channel bleeds off, for everything that has one.
//
// Deliberately far below the ship's linearDamping: that number is the flight
// model's feel and gets retuned, and a blast should not get shorter every time
// the ship is made to coast further.
inline constexpr float kKnockbackDamping = 1.6f;   // 1/s

// A ship at or below this fraction of its health is disabled: it stops flying
// itself and drifts on whatever momentum it had, tumbling. Robots and the
// player alike - the same number governs both, because it is the same idea.
inline constexpr float kDisabledHealthFraction = 0.10f;

// Adds an impulse to a body. `strength` is the velocity change it would impart
// at mass 1, so heavier things shrug the same hit off. `direction` need not be
// normalised.
inline void applyImpulse(Body& body, const glm::vec3& direction, float strength, float mass) {
    if (strength <= 0.0f || mass <= 0.0f) return;
    const float length = glm::length(direction);
    if (length < 1e-6f) return;
    body.knockback += direction * (strength / (length * mass));
}

} // namespace dvk::sim
