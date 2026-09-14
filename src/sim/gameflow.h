#pragma once

#include "game/level.h"
#include "sim/body.h"

#include <array>
#include <cstdint>

namespace dvk::sim {

struct CombatState;
struct RobotState;
struct RobotConfig;

// Which side of a segment is currently shut. One array, consulted by the portal
// walk, the light flood fill and collision alike, so those three can never
// disagree about whether a door is in the way - which is exactly the
// disagreement that produces light shining through a closed door.
inline constexpr int kMaxFlowSegments = 128;

struct DoorField {
    std::array<uint8_t, kMaxFlowSegments * SideCount> blocked{};

    bool isBlocked(int segment, int side) const {
        const size_t i = static_cast<size_t>(segment) * SideCount + static_cast<size_t>(side);
        return i < blocked.size() && blocked[i] != 0;
    }
};

struct DoorState {
    float openness = 0.0f;   // 0 shut, 1 fully open
    float holdTimer = 0.0f;
    bool wantOpen = false;
    bool everOpened = false;
};

enum class GamePhase : uint8_t {
    Playing,      // find the reactor
    Countdown,    // reactor gone, get out
    Won,
    Lost,
};

inline constexpr int kMaxDoors = 32;
inline constexpr int kMaxPickups = 96;

struct GameState {
    GamePhase phase = GamePhase::Playing;
    float countdown = 0.0f;         // seconds left once the reactor is gone
    float elapsed = 0.0f;           // seconds since the level started
    float shake = 0.0f;             // 0..1, how hard the mine is coming apart
    int robotsDestroyed = 0;
    int robotsAtStart = 0;
    bool reactorAlive = false;
    bool exitOpen = false;
    const char* endReason = "";

    std::array<DoorState, kMaxDoors> doors{};
    std::array<bool, kMaxPickups> collected{};
    std::array<bool, 3> keys{};     // blue, yellow, red

    DoorField doorField;
};

struct GameConfig {
    float countdownSeconds = 45.0f;
    float shieldPickup = 25.0f;
    float energyPickup = 30.0f;
    int concussionPickup = 3;
    int homingPickup = 3;
    int flarePickup = 4;
};

void initGame(const Level& level, const RobotState& robots, GameState& out);

// One fixed step of doors, pickups and the win/lose flow.
void stepGame(const Level& level, const GameConfig& cfg, GameState& state, CombatState& combat,
              RobotState& robots, const RobotConfig& robotCfg, Body& player, float dt);

// True if a body may pass from `segment` through `side`. Doors that are not
// nearly open are not passable.
bool sidePassable(const GameState& state, int segment, int side);

} // namespace dvk::sim
