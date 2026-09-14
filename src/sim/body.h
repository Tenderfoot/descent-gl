#pragma once

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
    float radius = 1.6f;
    // Which segment the body is in. Tracked by following portal planes rather
    // than searched for, so it costs O(portals crossed) and not O(segments).
    int segment = 0;
};

} // namespace dvk::sim
