#pragma once

#include "sim/triggers.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

struct lua_State;
struct lua_Debug;

namespace dvk {

struct Level;

namespace sim {
struct GameState;
struct PlayerState;
}

struct ScriptMessage {
    std::string id;
    std::string clip;
    std::string speaker;
    std::string text;
    float seconds = 4.5f;
};

// What the player is carrying the moment the level starts, as the script
// declares it.
//
// A declaration and not a call, which is the whole point. `loadout` is read
// while the script is still being loaded, before any SimState exists, so
// there is nothing for a set-this-now function to write to; and the state a
// level begins from is copied fresh over the live one by every start and
// every restart, so anything written to the live state at load would survive
// exactly until the run actually began. Recording the numbers here and
// letting the session owner put them on `LevelSession::initial()` is the only
// shape that survives a restart.
//
// A negative field is one the script never mentioned, and the PlayerState
// default stands. That is why these are signed and why zero is a real value:
// a level may quite reasonably start the ship with no energy, no missiles and
// no flares at all.
struct ScriptLoadout {
    float shields = -1.0f;
    float energy = -1.0f;
    int vulcanAmmo = -1;
    int concussionAmmo = -1;
    int homingAmmo = -1;
    int flares = -1;
};

// Copies whatever the loadout actually declared onto a player. Fields the
// script left alone are not touched.
void applyLoadout(const ScriptLoadout& loadout, sim::PlayerState& player);

// Owns the one Lua state for the loaded level. It executes only in the game
// layer: once while loading declarations and while draining trigger callbacks
// after the fixed-step accumulator. No Lua object crosses into sim or render.
class ScriptHost {
public:
    ScriptHost();
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    void load(const Level& level);
    void clear();
    void drain(sim::GameState& game);

    const sim::TriggerSet& triggers() const { return triggers_; }
    const sim::TimerSet& timers() const { return timers_; }
    const std::vector<ScriptMessage>& messages() const { return messages_; }
    const ScriptLoadout& loadout() const { return loadout_; }
    const ScriptMessage* message(int index) const;
    int takeMessage();
    // The shields a callback asked for this frame, or negative if none did.
    // Read once and cleared, exactly like takeMessage().
    float takeShields();
    bool loaded() const { return state_ != nullptr; }
    int callbackErrors() const { return callbackErrors_; }

private:
    struct MemoryBudget {
        size_t used = 0;
        size_t limit = 8u * 1024u * 1024u;
    };

    static void* allocate(void* ud, void* ptr, size_t oldSize, size_t newSize);
    static void instructionLimit(lua_State* state, lua_Debug* debug);
    static int declareMessage(lua_State* state);
    static int declareLoadout(lua_State* state);
    static int declareTrigger(lua_State* state);
    static int declareTimer(lua_State* state);
    static int startComm(lua_State* state);
    static int setShields(lua_State* state);

    static ScriptHost* self(lua_State* state);
    int findMessage(const char* id) const;
    bool call(int functionRef, const char* context);

    lua_State* state_ = nullptr;
    MemoryBudget memory_;
    const Level* level_ = nullptr;
    bool draining_ = false;
    int requestedMessage_ = -1;
    float requestedShields_ = -1.0f;
    ScriptLoadout loadout_;
    bool loadoutDeclared_ = false;
    sim::TriggerSet triggers_;
    sim::TimerSet timers_;
    std::array<int, sim::kMaxTriggers> callbacks_{};
    std::array<int, sim::kMaxTimers> timerCallbacks_{};
    std::vector<ScriptMessage> messages_;
    int callbackErrors_ = 0;
};

int runScriptTest(const std::string& levelPath);

} // namespace dvk
