#pragma once

#include "sim/body.h"

#include <array>
#include <glm/vec3.hpp>

namespace dvk {
struct Level;
}

namespace dvk::sim {

struct SolidDoors;

// Things in the mine that move but are not ships.
//
// Separate from Level::objects, which is authoring data and assumes nothing
// ever leaves the position it was placed at: a prop that moves needs simulation
// state, and simulation state is copied every step, so it lives here in
// fixed-capacity arrays held by value.
//
// Neither of these is a robot. They do not think, they cannot be killed, and
// they are not on anybody's side - they are furniture with a light in it.

// A light in a ball, floating in a room. Shooting it shoves it, and it bounces
// off the walls rather than sliding along them.
struct FloatingLight {
    bool alive = false;
    Body body;
    glm::vec3 color{1.0f, 0.86f, 0.62f};
    float lightRadius = 26.0f;
    float lightIntensity = 2.6f;
    // Above zero so it bounces. The collision response is shared with every
    // other body in the mine; this is the number that makes it behave
    // differently from a ship.
    float restitution = 0.72f;
    float mass = 0.45f;
    // A slow aimless wander, so an untouched ball is not a fixed light that
    // happens to be in the middle of the room.
    float drift = 1.4f;        // units/s
    float driftTimer = 0.0f;
    glm::vec3 driftAim{0.0f};
};

// A spotlight hanging from the ceiling on a cord, swinging.
//
// The swing is analytic: one sine, from which the lamp position, the spot
// direction and the cord's lower end all follow. A constraint solver would be
// the wrong tool - it can drift, it needs tuning to stay stable, and none of
// that buys anything for a lamp that only has to swing back and forth. If it
// ever has to react to being shot, the replacement is a one-dimensional damped
// pendulum, which is four lines and still not a physics library.
struct SwingingLight {
    bool alive = false;
    glm::vec3 anchor{0.0f};     // where the cord meets the ceiling
    int segment = 0;
    float cordLength = 7.0f;
    float amplitude = 0.55f;    // radians of swing either side of straight down
    float rate = 1.5f;          // rad/s through the sine, so the period is 2pi/rate
    float phase = 0.0f;
    glm::vec3 swingAxis{1.0f, 0.0f, 0.0f};   // the horizontal the lamp swings along
    glm::vec3 color{1.0f, 0.95f, 0.82f};
    float lightRadius = 34.0f;
    float lightIntensity = 3.2f;
    float coneDegrees = 60.0f;
    float time = 0.0f;

    // Derived every step from the sine above. Kept rather than recomputed by
    // each reader so the simulation and the renderer cannot disagree about
    // where the lamp is.
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
};

inline constexpr int kMaxFloatingLights = 8;
inline constexpr int kMaxSwingingLights = 8;

struct PropState {
    std::array<FloatingLight, kMaxFloatingLights> floating{};
    std::array<SwingingLight, kMaxSwingingLights> swinging{};
    int floatingCount = 0;
    int swingingCount = 0;
};

// Reads the level's authored props into simulation state.
void spawnProps(const Level& level, PropState& out);

// One fixed step. The balls integrate, collide and wander; the lamps advance
// their swing and recompute where they are.
void stepProps(const Level& level, PropState& state, float dt,
               const SolidDoors* doors = nullptr);

// Where a lamp's housing sits for a given swing angle. Shared so that the
// loader, the simulation and the validator all place it the same way.
glm::vec3 lampPosition(const SwingingLight& lamp, float angle);

} // namespace dvk::sim
