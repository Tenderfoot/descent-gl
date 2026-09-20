#pragma once

#include "game/state.h"
#include "game/settings.h"
#include "game/visibility.h"
#include "sim/collision.h"
#include "sim/lighting.h"

#include <glm/geometric.hpp>

#include <vector>

namespace dvk {

struct Level;
struct LightFlood;
struct Light;

namespace audio {
class Audio;
}

namespace sim {
struct EventQueue;
struct FlightConfig;
struct WeaponConfig;
}

inline constexpr float kDefaultAimDistance = 200.0f;

// The chase camera's state, held across frames because both halves of it are
// chased rather than set: see where they are updated in prepareRenderFrame.
//
// It starts fully extended and with the ship drawn, which is what an opening
// frame in clear air resolves to anyway. Starting it pulled in would dolly the
// camera out over the first half second of every level.
struct ChaseCamera {
    float distance = glm::length(kChaseOffset);
    bool shipDrawn = true;
};

struct FramePrepareContext {
    const SimState& previous;
    SimState& current;
    float alpha;
    CameraMode camera;
    const PlayerSettings& settings;
    double nowSeconds;
    double frameSeconds;
    audio::Audio& audio;
    const sim::EventQueue& soundEvents;
    Level& level;
    const sim::FlightConfig& flight;
    const sim::WeaponConfig& weapons;
    LightFlood& flood;
    int& lightDoorSignature;
    std::vector<Light>& dynamicLights;
    const sim::SolidDoors& solidDoors;
    const sim::OccluderSet& occluders;
    sim::Illumination& playerLight;
    sim::PlayerVisibility& playerSeen;
    float& aimDistance;
    ChaseCamera& chase;
};

struct PreparedFrame {
    RenderState renderState;
    glm::vec3 shipForward{0.0f, 0.0f, -1.0f};
    sim::RayHit noseRay;
};

PreparedFrame prepareRenderFrame(FramePrepareContext& context);

} // namespace dvk
