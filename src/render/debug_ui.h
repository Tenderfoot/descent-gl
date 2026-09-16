#pragma once

#include <glm/vec2.hpp>

#include <cstdint>
#include <string>

namespace dvk {

class Input;
struct Level;
struct LightFlood;
class PortalWalk;
struct Options;
struct RenderState;
struct SimState;
enum class CameraMode;

namespace audio {
class Audio;
}

namespace sim {
struct BodyResolveStats;
struct BodySet;
struct ContactLog;
struct FlightConfig;
struct Illumination;
struct PlayerVisibility;
struct RayHit;
struct RobotConfig;
struct WeaponConfig;
}

namespace gl {

class Renderer;

struct DebugUiContext {
    double frameMs;
    long long totalSteps;
    double simHz;
    float alpha;
    const glm::ivec2& framebuffer;
    Renderer& renderer;
    RenderState& renderState;
    Level& level;
    PortalWalk& walk;
    bool& freezeVisible;
    sim::FlightConfig& flight;
    const sim::FlightConfig& flightDefaults;
    CameraMode camera;
    const Input& input;
    SimState& current;
    const sim::ContactLog& contacts;
    const sim::BodySet& bodies;
    const sim::BodyResolveStats& bodyStats;
    const sim::RayHit& noseRay;
    int frame;
    int allocWarmupFrames;
    uint64_t worstSimAllocs;
    int simAllocFrames;
    uint64_t worstDrawAllocs;
    int drawAllocFrames;
    const sim::Illumination& playerLight;
    const sim::PlayerVisibility& playerSeen;
    sim::WeaponConfig& weapons;
    const sim::RobotConfig& robotConfig;
    audio::Audio& audio;
    const Options& options;
    LightFlood& flood;
    const std::string& levelFile;
    int& lightEditorSelection;
    const char* flightConfigPath;
    const char* weaponConfigPath;
};

void drawDebugUi(DebugUiContext& context);

} // namespace gl
} // namespace dvk
