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
struct CombatState;
struct GameState;
struct PlayerState;
struct RobotConfig;
struct RobotState;
}

// The rest of the run, for the questions a callback is allowed to ask. The
// GameState is not in here because drain() is handed that already; these are
// the three things it is not.
//
// Pointers rather than references because not every caller has a mine around
// it: the script test drains levels with no ship and no robots in them, and a
// query with nothing to read should say so rather than invent an answer.
struct ScriptWorld {
    const sim::CombatState* combat = nullptr;
    const sim::RobotState* robots = nullptr;
    const sim::RobotConfig* robotConfig = nullptr;
};

struct ScriptMessage {
    std::string id;
    std::string clip;
    std::string speaker;
    std::string text;
    float seconds = 4.5f;
};

// A tutorial pane: a looping clip, a heading and a body, shown with the game
// stopped until the player acknowledges it.
//
// Deliberately not a ScriptMessage with a flag on it. A transmission has a
// display time and retires itself; a tutorial has neither, because what ends
// it is the player rather than the clock. They also arrive on different
// surfaces - one is a corner of the HUD the player flies past, the other is a
// modal that takes the mouse - and a field that is meaningless on one of them
// is a field somebody will eventually set.
struct ScriptTutorial {
    std::string id;
    std::string clip;
    std::string title;
    std::string text;
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
    // The ship's shield ceiling for the level; see PlayerState::maxShields.
    float maxShields = -1.0f;
    float shields = -1.0f;
    float energy = -1.0f;
    int vulcanAmmo = -1;
    int concussionAmmo = -1;
    int homingAmmo = -1;
    int railgunAmmo = -1;
    int lightningAmmo = -1;
    int flares = -1;
    // Whether a disabled ship repairs itself; -1 unmentioned, else 0 or 1.
    // Off for a level whose script holds the ship disabled through an opening
    // and hands it back with set_self_repair(true) when it is done.
    int selfRepair = -1;
    // Seconds to get out once the reactor is gone, for this level only; -1
    // leaves the game's default. Not about the ship, but it is a condition the
    // run starts from, which is what a loadout is.
    float countdown = -1.0f;
};

// Copies whatever the loadout actually declared onto a player. Fields the
// script left alone are not touched.
void applyLoadout(const ScriptLoadout& loadout, sim::PlayerState& player);

// How many delayed callbacks can be outstanding at once. A level with sixteen
// beats in the air at the same moment is already doing something stranger than
// the mine can express, and a fixed array is what keeps a runtime-scheduled
// call out of the heap.
inline constexpr int kMaxDeferredCalls = 16;

// How many scripted responses to the run's own moments a level may declare.
// Small because there are only three moments to respond to, and a level with
// sixteen things to say about them is a level with a pacing problem.
inline constexpr int kMaxEventHooks = 16;

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
    // `world` is what the query functions read. A drain without one still
    // runs every callback; only the queries notice, and they refuse rather
    // than answering about a mine that is not there.
    void drain(sim::GameState& game, const ScriptWorld& world = {});

    const sim::TriggerSet& triggers() const { return triggers_; }
    const sim::TimerSet& timers() const { return timers_; }
    const sim::PickupHookSet& pickupHooks() const { return pickupHooks_; }
    const std::vector<ScriptMessage>& messages() const { return messages_; }
    const std::vector<ScriptTutorial>& tutorials() const { return tutorials_; }
    const ScriptLoadout& loadout() const { return loadout_; }
    const ScriptMessage* message(int index) const;
    const ScriptTutorial* tutorial(int index) const;
    int takeMessage();
    // The tutorial a callback asked for this frame, or -1. Read once and
    // cleared, exactly like takeMessage(). Only one can be latched per drain:
    // two panes in one frame would mean the second replaced a pane the player
    // never saw, so the first one wins and the second is logged.
    int takeTutorial();
    // The shields a callback asked for this frame, or negative if none did.
    // Read once and cleared, exactly like takeMessage().
    float takeShields();
    // Whether a callback switched self-repair this frame: 1 on, 0 off, -1 if
    // none did. Read once and cleared, exactly like takeShields().
    int takeSelfRepair();
    // The keys callbacks handed over this frame, as a mask with bit i for
    // kKeyNames[i], or 0. Read once and cleared, exactly like takeShields().
    unsigned takeKeys();
    bool loaded() const { return state_ != nullptr; }
    int callbackErrors() const { return callbackErrors_; }

private:
    struct MemoryBudget {
        size_t used = 0;
        size_t limit = 8u * 1024u * 1024u;
    };

    // A script's response to one of the run's own moments. No `once`: each
    // moment happens at most once in a run, so the sim state that records it
    // is already the only latch there is to keep.
    struct EventHook {
        sim::LevelEvent kind = sim::LevelEvent::ReactorDestroyed;
        int callback = 0;
    };

    // One call a callback asked to have made later. `due` is on the level's
    // own clock, the same seconds-since-start a timer is authored against, so
    // a delay does not run down while the game is paused or while a tutorial
    // pane has the simulation stopped - which is the whole point of measuring
    // it there rather than against the wall clock.
    struct Deferred {
        float due = 0.0f;
        int callback = 0;
        bool active = false;
    };

    static void* allocate(void* ud, void* ptr, size_t oldSize, size_t newSize);
    static void instructionLimit(lua_State* state, lua_Debug* debug);
    static int declareMessage(lua_State* state);
    static int declareLoadout(lua_State* state);
    static int declareTrigger(lua_State* state);
    static int declareTimer(lua_State* state);
    static int declarePickup(lua_State* state);
    static int declareEvent(lua_State* state);

    // --- queries ----------------------------------------------------------
    //
    // Reads, and a read is the one thing a script can do to the simulation
    // without latching. comm() and set_shields() defer because Lua runs
    // outside the fixed step and must not write simulation state; looking at
    // state that has already been stepped breaks no such rule, so these
    // answer on the spot.
    //
    // All of them are callback-only, for the same reason comm() is: at load
    // there is no run to ask about.
    static int queryPhase(lua_State* state);
    static int queryElapsed(lua_State* state);
    static int queryCountdown(lua_State* state);
    static int queryReactorAlive(lua_State* state);
    static int queryRobotsLeft(lua_State* state);
    static int queryRobotsAlive(lua_State* state);
    static int queryHasKey(lua_State* state);
    static int queryShields(lua_State* state);
    static int queryEnergy(lua_State* state);
    static int queryLaserLevel(lua_State* state);
    static int queryAmmo(lua_State* state);
    static int startComm(lua_State* state);
    static int declareTutorial(lua_State* state);
    static int showTutorial(lua_State* state);
    static int setShields(lua_State* state);
    static int setSelfRepair(lua_State* state);
    static int giveKey(lua_State* state);
    static int scheduleAfter(lua_State* state);

    static ScriptHost* self(lua_State* state);
    int findMessage(const char* id) const;
    int findTutorial(const char* id) const;
    // `argument`, when given, is pushed as the callback's one parameter. It is
    // how a pickup hook is told which kind was taken; a trigger or a timer has
    // nothing to say and passes nullptr.
    bool call(int functionRef, const char* context, const char* argument = nullptr);
    // The common preamble of every query: refuses outside a callback, and
    // hands back what there is to read. Never returns when it refuses.
    const sim::GameState& queryGame(lua_State* state, const char* name) const;
    // The same for the player and the robots, which not every drain has.
    const sim::PlayerState& queryPlayer(lua_State* state, const char* name) const;
    const sim::RobotState& queryRobots(lua_State* state, const char* name) const;
    // Drops every outstanding delayed call and releases its reference. What a
    // restart does to the beats a previous run left in the air.
    void cancelDeferred();
    void runDeferred();

    lua_State* state_ = nullptr;
    MemoryBudget memory_;
    const Level* level_ = nullptr;
    bool draining_ = false;
    int requestedMessage_ = -1;
    int requestedTutorial_ = -1;
    float requestedShields_ = -1.0f;
    int requestedSelfRepair_ = -1;
    unsigned requestedKeys_ = 0;
    ScriptLoadout loadout_;
    bool loadoutDeclared_ = false;
    sim::TriggerSet triggers_;
    sim::TimerSet timers_;
    sim::PickupHookSet pickupHooks_;
    std::array<int, sim::kMaxTriggers> callbacks_{};
    std::array<int, sim::kMaxTimers> timerCallbacks_{};
    std::array<int, sim::kMaxPickupHooks> pickupCallbacks_{};
    std::array<EventHook, kMaxEventHooks> eventHooks_{};
    int eventHookCount_ = 0;
    std::array<Deferred, kMaxDeferredCalls> deferred_{};
    // What the queries read. Valid only for the length of a drain, which is
    // the only time a query can be asked.
    const sim::GameState* queryGame_ = nullptr;
    ScriptWorld world_;
    // The level clock as of this drain, and as of the last one. The first is
    // what after() counts from; the second is how a restart is noticed, since
    // a clock that has gone backwards can only mean the run began again and
    // the delays the last one scheduled are for a game that no longer exists.
    float elapsed_ = 0.0f;
    float lastElapsed_ = 0.0f;
    std::vector<ScriptMessage> messages_;
    std::vector<ScriptTutorial> tutorials_;
    int callbackErrors_ = 0;
};

int runScriptTest(const std::string& levelPath);

} // namespace dvk
