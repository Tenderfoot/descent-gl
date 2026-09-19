#include "game/script.h"

#include "core/log.h"
#include "core/alloc_count.h"
#include "game/level.h"
#include "sim/combat.h"
#include "sim/gameflow.h"

#include <lua.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace dvk {

namespace {

constexpr int kInstructionBudget = 100'000;
constexpr size_t kMaxMessageId = 64;
constexpr size_t kMaxClipName = 64;
constexpr size_t kMaxSpeaker = 32;
constexpr size_t kMaxText = 512;

// One optional number out of a declaration table. Absent leaves `out`
// untouched, which is how a loadout field the script never mentioned keeps
// the PlayerState default rather than being zeroed.
bool optionalNumber(lua_State* state, int table, const char* field, const char* what, double low,
                    double high, double& out) {
    lua_getfield(state, table, field);
    const bool present = !lua_isnil(state, -1);
    if (present) {
        const double value = luaL_checknumber(state, -1);
        if (!std::isfinite(value) || value < low || value > high) {
            lua_pop(state, 1);
            luaL_error(state, "loadout %s must be between %g and %g", what, low, high);
        }
        out = value;
    }
    lua_pop(state, 1);
    return present;
}

const char* requiredString(lua_State* state, int table, const char* field) {
    lua_getfield(state, table, field);
    const char* value = luaL_checkstring(state, -1);
    lua_pop(state, 1);
    return value;
}

void removeGlobal(lua_State* state, const char* name) {
    lua_pushnil(state);
    lua_setglobal(state, name);
}

void expose(lua_State* state, ScriptHost* host, const char* name, lua_CFunction function) {
    lua_pushlightuserdata(state, host);
    lua_pushcclosure(state, function, 1);
    lua_setglobal(state, name);
}

} // namespace

ScriptHost::ScriptHost() {
    callbacks_.fill(LUA_NOREF);
}

ScriptHost::~ScriptHost() {
    clear();
}

void* ScriptHost::allocate(void* ud, void* ptr, size_t oldSize, size_t newSize) {
    auto& memory = *static_cast<MemoryBudget*>(ud);
    if (newSize == 0) {
        std::free(ptr);
        memory.used = oldSize <= memory.used ? memory.used - oldSize : 0;
        return nullptr;
    }

    const size_t kept = ptr ? oldSize : 0;
    const size_t base = kept <= memory.used ? memory.used - kept : 0;
    if (newSize > memory.limit - std::min(memory.limit, base)) return nullptr;
    void* next = std::realloc(ptr, newSize);
    if (next) memory.used = base + newSize;
    return next;
}

void ScriptHost::instructionLimit(lua_State* state, lua_Debug*) {
    luaL_error(state, "script instruction budget exceeded");
}

ScriptHost* ScriptHost::self(lua_State* state) {
    return static_cast<ScriptHost*>(lua_touserdata(state, lua_upvalueindex(1)));
}

void ScriptHost::clear() {
    draining_ = false;
    requestedMessage_ = -1;
    level_ = nullptr;
    if (state_) lua_close(state_);
    state_ = nullptr;
    memory_.used = 0;
    loadout_ = ScriptLoadout{};
    loadoutDeclared_ = false;
    triggers_ = sim::TriggerSet{};
    callbacks_.fill(LUA_NOREF);
    messages_.clear();
    callbackErrors_ = 0;
}

void ScriptHost::load(const Level& level) {
    clear();
    if (level.scriptPath.empty()) return;

    level_ = &level;
#if LUA_VERSION_NUM >= 505
    state_ = lua_newstate(&ScriptHost::allocate, &memory_, 0);
#else
    state_ = lua_newstate(&ScriptHost::allocate, &memory_);
#endif
    if (!state_) log::fatal("could not create Lua state for '{}'", level.scriptPath);
    *static_cast<ScriptHost**>(lua_getextraspace(state_)) = this;

    luaL_requiref(state_, LUA_GNAME, luaopen_base, 1);
    lua_pop(state_, 1);
    luaL_requiref(state_, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(state_, 1);
    luaL_requiref(state_, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(state_, 1);
    luaL_requiref(state_, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(state_, 1);

    removeGlobal(state_, "collectgarbage");
    removeGlobal(state_, "dofile");
    removeGlobal(state_, "load");
    removeGlobal(state_, "loadfile");

    expose(state_, this, "loadout", &ScriptHost::declareLoadout);
    expose(state_, this, "message", &ScriptHost::declareMessage);
    expose(state_, this, "trigger", &ScriptHost::declareTrigger);
    expose(state_, this, "comm", &ScriptHost::startComm);

    if (luaL_loadfile(state_, level.scriptPath.c_str()) != LUA_OK) {
        const char* error = lua_tostring(state_, -1);
        log::fatal("Lua script '{}' would not compile: {}", level.scriptPath,
                   error ? error : "unknown error");
    }
    lua_sethook(state_, &ScriptHost::instructionLimit, LUA_MASKCOUNT, kInstructionBudget);
    const int status = lua_pcall(state_, 0, 0, 0);
    lua_sethook(state_, nullptr, 0, 0);
    if (status != LUA_OK) {
        const char* error = lua_tostring(state_, -1);
        log::fatal("Lua script '{}' failed while declaring content: {}", level.scriptPath,
                   error ? error : "unknown error");
    }

    log::info("Lua script '{}': {} trigger(s), {} message(s)", level.scriptPath,
              triggers_.count, messages_.size());
}

int ScriptHost::declareMessage(lua_State* state) {
    ScriptHost* host = self(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const char* id = requiredString(state, 1, "id");
    const char* clip = requiredString(state, 1, "clip");
    const char* speaker = requiredString(state, 1, "speaker");
    const char* text = requiredString(state, 1, "text");

    lua_getfield(state, 1, "seconds");
    const float seconds = lua_isnil(state, -1) ? 4.5f : static_cast<float>(luaL_checknumber(state, -1));
    lua_pop(state, 1);

    if (std::strlen(id) == 0 || std::strlen(id) > kMaxMessageId) {
        return luaL_error(state, "message id must contain 1..%zu characters", kMaxMessageId);
    }
    if (std::strlen(clip) == 0 || std::strlen(clip) > kMaxClipName) {
        return luaL_error(state, "clip must contain 1..%zu characters", kMaxClipName);
    }
    if (std::strlen(speaker) > kMaxSpeaker || std::strlen(text) > kMaxText) {
        return luaL_error(state, "message speaker or text is too long");
    }
    if (!std::isfinite(seconds) || seconds < 0.1f || seconds > 60.0f) {
        return luaL_error(state, "message seconds must be finite and between 0.1 and 60");
    }
    if (host->findMessage(id) >= 0) return luaL_error(state, "duplicate message id '%s'", id);

    host->messages_.push_back({id, clip, speaker, text, seconds});
    return 0;
}

int ScriptHost::declareLoadout(lua_State* state) {
    ScriptHost* host = self(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    if (host->loadoutDeclared_) return luaL_error(state, "a level declares at most one loadout");

    // Shields are the one field with no zero: at zero the level is already
    // lost, and a level that starts lost is a level file bug rather than an
    // authoring choice. Everything between there and the disabled threshold
    // is legitimate - that band is how a level starts the ship crippled.
    double value = 0.0;
    if (optionalNumber(state, 1, "shields", "shields", 0.01,
                       static_cast<double>(sim::kPlayerMaxShields), value)) {
        host->loadout_.shields = static_cast<float>(value);
    }
    if (optionalNumber(state, 1, "energy", "energy", 0.0, 100.0, value)) {
        host->loadout_.energy = static_cast<float>(value);
    }
    if (optionalNumber(state, 1, "vulcan", "vulcan ammo", 0.0, 50'000.0, value)) {
        host->loadout_.vulcanAmmo = static_cast<int>(value);
    }
    if (optionalNumber(state, 1, "concussion", "concussion ammo", 0.0, 999.0, value)) {
        host->loadout_.concussionAmmo = static_cast<int>(value);
    }
    if (optionalNumber(state, 1, "homing", "homing ammo", 0.0, 999.0, value)) {
        host->loadout_.homingAmmo = static_cast<int>(value);
    }
    if (optionalNumber(state, 1, "flares", "flare count", 0.0, 999.0, value)) {
        host->loadout_.flares = static_cast<int>(value);
    }

    host->loadoutDeclared_ = true;
    return 0;
}

int ScriptHost::declareTrigger(lua_State* state) {
    ScriptHost* host = self(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    if (host->triggers_.count >= sim::kMaxTriggers) {
        return luaL_error(state, "level exceeds the limit of %d triggers", sim::kMaxTriggers);
    }

    sim::TriggerVolume trigger;
    lua_getfield(state, 1, "segment");
    const bool hasSegment = !lua_isnil(state, -1);
    if (hasSegment) trigger.segment = static_cast<int>(luaL_checkinteger(state, -1));
    lua_pop(state, 1);

    lua_getfield(state, 1, "position");
    const bool hasPosition = !lua_isnil(state, -1);
    if (hasSegment == hasPosition) {
        return luaL_error(state, "trigger needs exactly one of segment or position");
    }
    if (hasPosition) {
        luaL_checktype(state, -1, LUA_TTABLE);
        for (int i = 0; i < 3; ++i) {
            lua_rawgeti(state, -1, i + 1);
            trigger.position[static_cast<glm::length_t>(i)] =
                static_cast<float>(luaL_checknumber(state, -1));
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);

    if (hasSegment) {
        trigger.shape = sim::TriggerShape::Segment;
        if (trigger.segment < 0 || trigger.segment >= host->level_->segmentCount()) {
            return luaL_error(state, "trigger segment %d is outside the level", trigger.segment);
        }
    } else {
        trigger.shape = sim::TriggerShape::Sphere;
        lua_getfield(state, 1, "radius");
        trigger.radius = static_cast<float>(luaL_checknumber(state, -1));
        lua_pop(state, 1);
        if (!std::isfinite(trigger.radius) || trigger.radius <= 0.0f || trigger.radius > 1000.0f) {
            return luaL_error(state, "trigger radius must be finite and between 0 and 1000");
        }
        if (!std::isfinite(trigger.position.x) || !std::isfinite(trigger.position.y) ||
            !std::isfinite(trigger.position.z)) {
            return luaL_error(state, "trigger position must be finite");
        }
        trigger.segment = findSegment(*host->level_, trigger.position);
        if (trigger.segment < 0) return luaL_error(state, "trigger position is not in a segment");
    }

    lua_getfield(state, 1, "once");
    if (!lua_isnil(state, -1)) trigger.once = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);

    lua_getfield(state, 1, "on_enter");
    if (!lua_isfunction(state, -1)) return luaL_error(state, "trigger on_enter must be a function");
    const int callback = luaL_ref(state, LUA_REGISTRYINDEX);

    const int index = host->triggers_.count++;
    host->triggers_.items[static_cast<size_t>(index)] = trigger;
    host->callbacks_[static_cast<size_t>(index)] = callback;
    return 0;
}

int ScriptHost::startComm(lua_State* state) {
    ScriptHost* host = self(state);
    const char* id = luaL_checkstring(state, 1);
    if (!host->draining_) return luaL_error(state, "comm() is only valid inside a callback");
    const int messageIndex = host->findMessage(id);
    if (messageIndex < 0) return luaL_error(state, "unknown message id '%s'", id);

    host->requestedMessage_ = messageIndex;
    return 0;
}

int ScriptHost::findMessage(const char* id) const {
    for (size_t i = 0; i < messages_.size(); ++i) {
        if (messages_[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

bool ScriptHost::call(int functionRef, const char* context) {
    lua_rawgeti(state_, LUA_REGISTRYINDEX, functionRef);
    lua_sethook(state_, &ScriptHost::instructionLimit, LUA_MASKCOUNT, kInstructionBudget);
    const int status = lua_pcall(state_, 0, 0, 0);
    lua_sethook(state_, nullptr, 0, 0);
    if (status == LUA_OK) return true;

    const char* error = lua_tostring(state_, -1);
    log::error("Lua {} failed and has been disabled: {}", context,
               error ? error : "unknown error");
    ++callbackErrors_;
    lua_pop(state_, 1);
    return false;
}

void ScriptHost::drain(sim::GameState& game) {
    if (!state_) {
        game.triggers.clearPending();
        return;
    }

    draining_ = true;
    const int count = game.triggers.pendingCount;
    for (int i = 0; i < count; ++i) {
        const int trigger = game.triggers.pending[static_cast<size_t>(i)];
        int& callback = callbacks_[static_cast<size_t>(trigger)];
        if (callback == LUA_NOREF) continue;
        if (!call(callback, "trigger callback")) {
            luaL_unref(state_, LUA_REGISTRYINDEX, callback);
            callback = LUA_NOREF;
        }
    }
    draining_ = false;
    game.triggers.clearPending();
}

const ScriptMessage* ScriptHost::message(int index) const {
    if (index < 0 || static_cast<size_t>(index) >= messages_.size()) return nullptr;
    return &messages_[static_cast<size_t>(index)];
}

int ScriptHost::takeMessage() {
    const int message = requestedMessage_;
    requestedMessage_ = -1;
    return message;
}

static int runOneScriptTest(const std::string& levelPath) {
    Level level = loadLevel(levelPath);
    ScriptHost host;
    host.load(level);
    if (!host.loaded()) {
        log::info("script-test: '{}' has no script: passed", levelPath);
        return 0;
    }

    bool ok = host.triggers().count > 0;
    sim::Body player;
    for (int i = 0; i < host.triggers().count; ++i) {
        sim::GameState game;
        const sim::TriggerVolume& trigger = host.triggers().items[static_cast<size_t>(i)];
        player.segment = trigger.segment;
        player.position = trigger.shape == sim::TriggerShape::Sphere
                              ? trigger.position + glm::vec3(trigger.radius * 2.0f, 0.0f, 0.0f)
                              : glm::vec3(0.0f);
        sim::stepTriggers(game.triggers, player, &host.triggers());

        player.position = trigger.position;
        sim::stepTriggers(game.triggers, player, &host.triggers());
        bool found = false;
        for (int p = 0; p < game.triggers.pendingCount; ++p) {
            found |= game.triggers.pending[static_cast<size_t>(p)] == i;
        }
        ok &= found;
        host.drain(game);

        // Staying inside is never a second entry. Leaving and returning is a
        // second entry only for a repeatable trigger.
        sim::stepTriggers(game.triggers, player, &host.triggers());
        ok &= game.triggers.pendingCount == 0;
        player.segment = -1;
        sim::stepTriggers(game.triggers, player, &host.triggers());
        player.segment = trigger.segment;
        sim::stepTriggers(game.triggers, player, &host.triggers());
        const bool repeated = game.triggers.pendingCount > 0;
        ok &= repeated == !trigger.once;
        host.drain(game);
    }

    sim::TriggerState allocationState;
    sim::Body allocationPlayer;
    allocationPlayer.segment = -1;
    alloc::Scope allocations;
    for (int i = 0; i < 10'000; ++i) {
        sim::stepTriggers(allocationState, allocationPlayer, &host.triggers());
    }
    const uint64_t allocationCount = allocations.count();
    ok &= allocationCount == 0;
    ok &= host.callbackErrors() == 0;

    if (!ok) {
        log::error("script-test: trigger entry, callback, or allocation check failed");
        return 1;
    }
    log::info("script-test: {} trigger(s), callbacks healthy, 10000 evaluations and {} allocation(s): passed",
              host.triggers().count, allocationCount);
    return 0;
}

void applyLoadout(const ScriptLoadout& loadout, sim::PlayerState& player) {
    if (loadout.shields >= 0.0f) player.shields = loadout.shields;
    if (loadout.energy >= 0.0f) player.energy = loadout.energy;
    if (loadout.vulcanAmmo >= 0) player.vulcanAmmo = loadout.vulcanAmmo;
    if (loadout.concussionAmmo >= 0) player.concussionAmmo = loadout.concussionAmmo;
    if (loadout.homingAmmo >= 0) player.homingAmmo = loadout.homingAmmo;
    if (loadout.flares >= 0) player.flares = loadout.flares;
}

int runScriptTest(const std::string& levelPath) {
    if (!levelPath.empty()) return runOneScriptTest(levelPath);

    std::vector<std::filesystem::path> levels;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator("assets/levels")) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            levels.push_back(entry.path());
        }
    }
    std::sort(levels.begin(), levels.end());

    int failures = 0;
    for (const std::filesystem::path& path : levels) failures += runOneScriptTest(path.string());
    if (failures == 0) {
        log::info("script-test: all {} shipped level(s) passed", levels.size());
    }
    return failures == 0 ? 0 : 1;
}

} // namespace dvk
