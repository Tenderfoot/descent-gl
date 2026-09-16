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
}

struct ScriptMessage {
    std::string id;
    std::string clip;
    std::string speaker;
    std::string text;
    float seconds = 4.5f;
};

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
    const ScriptMessage* message(int index) const;
    int takeMessage();
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
    static int declareTrigger(lua_State* state);
    static int startComm(lua_State* state);

    static ScriptHost* self(lua_State* state);
    int findMessage(const char* id) const;
    bool call(int functionRef, const char* context);

    lua_State* state_ = nullptr;
    MemoryBudget memory_;
    const Level* level_ = nullptr;
    bool draining_ = false;
    int requestedMessage_ = -1;
    sim::TriggerSet triggers_;
    std::array<int, sim::kMaxTriggers> callbacks_{};
    std::vector<ScriptMessage> messages_;
    int callbackErrors_ = 0;
};

int runScriptTest(const std::string& levelPath);

} // namespace dvk
