#pragma once

#include "game/level.h"
#include "sim/body.h"
#include "sim/events.h"
#include "sim/triggers.h"

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>

namespace dvk::sim {

struct CombatState;
struct RobotState;
struct RobotConfig;
struct SolidDoors;

// Which side of a segment is currently shut, in the two senses a door has.
//
// `blocked` is what a body cannot get through: a door counts as passable a
// little before it is fully open. `opaque` is what nothing can be seen or lit
// through: only a door that is FULLY shut. The moment a door starts to open
// the room behind it comes into view through the gap, while the slab still
// stops the ship for most of its travel. Both arrays are written together
// from the same door state, so the portal walk, the light flood and collision
// can never disagree about the same door.
inline constexpr int kMaxFlowSegments = 128;

struct DoorField {
    std::array<uint8_t, kMaxFlowSegments * SideCount> blocked{};
    std::array<uint8_t, kMaxFlowSegments * SideCount> opaque{};

    bool isBlocked(int segment, int side) const {
        const size_t i = static_cast<size_t>(segment) * SideCount + static_cast<size_t>(side);
        return i < blocked.size() && blocked[i] != 0;
    }
    bool isOpaque(int segment, int side) const {
        const size_t i = static_cast<size_t>(segment) * SideCount + static_cast<size_t>(side);
        return i < opaque.size() && opaque[i] != 0;
    }
    int opaqueCount() const {
        int n = 0;
        for (uint8_t v : opaque) n += v;
        return n;
    }
};

struct DoorState {
    float openness = 0.0f;   // 0 shut, 1 fully open
    float holdTimer = 0.0f;
    bool wantOpen = false;
    bool everOpened = false;
    // Rate limit for the "you do not have this key" refusal. The player's
    // distance to the door is tested every step, so without this, sitting in
    // front of a locked door buzzes 120 times a second.
    float lockedTimer = 0.0f;
};

// How long after a locked-door refusal before another one is allowed.
inline constexpr float kLockedRefusalInterval = 1.2f;

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

    // What the player has found, for the automap. Knowledge, not world state:
    // nothing here feeds back into the simulation, and a level plays out
    // identically whether the map is ever opened or not.
    //
    // `visited` is segments the ship has actually been inside - not segments it
    // has seen down a corridor, which would hand the player the layout of a room
    // they only glanced into. `seen` is a pickup whose segment has been visited,
    // so the map can show a key the player passed and left behind.
    std::array<bool, kMaxFlowSegments> visited{};
    std::array<bool, kMaxPickups> seen{};
    int visitedCount = 0;
    bool reactorSeen = false;
    glm::vec3 reactorPosition{0.0f};   // last known, valid once reactorSeen

    DoorField doorField;
    TriggerState triggers;
    // The level's scripted clock. Here rather than in the script host so that
    // a restart rewinds it with everything else and the intro plays again.
    TimerState timers;
};

struct GameConfig {
    float countdownSeconds = 45.0f;
    float shieldPickup = 25.0f;
    float energyPickup = 30.0f;
    int concussionPickup = 3;
    int homingPickup = 3;
    int railgunPickup = 10;
    int lightningPickup = 100;
    int flarePickup = 4;
};

void initGame(const Level& level, const RobotState& robots, GameState& out);

// One fixed step of doors, pickups and the win/lose flow. `events` is the sound
// queue and may be null. See sim/events.h.
void stepGame(const Level& level, const GameConfig& cfg, GameState& state, CombatState& combat,
              RobotState& robots, const RobotConfig& robotCfg, Body& player, float dt,
              EventQueue* events = nullptr, const TriggerSet* triggers = nullptr,
              const TimerSet* timers = nullptr);

// True if a body may pass from `segment` through `side`. Doors that are not
// nearly open are not passable.
bool sidePassable(const GameState& state, int segment, int side);

// The world-space faces of every door that still blocks a body. Each face also
// records whether the door is fully opaque, so line of sight and projectiles
// can open at the first crack with the portal walk and light flood while body
// collision keeps its larger clearance threshold.
void buildSolidDoors(const Level& level, const GameState& state, SolidDoors& out);

} // namespace dvk::sim
