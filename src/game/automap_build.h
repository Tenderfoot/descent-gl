#pragma once

#include <glm/vec3.hpp>

namespace dvk {

struct Level;
struct RenderState;
struct SimState;

namespace gl {
struct AutomapData;
}

namespace sim {
struct RobotConfig;
}

// Fills in what the map draws, for the same reason buildHud does: the renderer
// is handed a finished view and never reaches into GameState.
//
// Only the fields that are a function of the simulation are written. Where the
// player has put the camera - open, yaw, pitch, zoom - belongs to the map and
// survives this untouched.
//
// `reveal` is --reveal: it shades every segment as visited and shows every
// marker. A rendering check for the map itself, in the same family as
// --wireframe-volumes; the game still only records the rooms actually flown
// through.
void buildAutomap(gl::AutomapData& map, const Level& level, const SimState& state,
                  const RenderState& render, const glm::vec3& shipForward,
                  const sim::RobotConfig& robots, bool reveal);

} // namespace dvk
