#include "game/script.h"

#include "core/log.h"
#include "core/alloc_count.h"
#include "game/level.h"
#include "sim/combat.h"
#include "sim/gameflow.h"
#include "sim/robots.h"

#include <lua.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
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
// A tutorial pane is a page rather than a corner of the HUD, so it is allowed
// more than a transmission's caption. Still bounded: the pane wraps what it is
// given and draws what fits, and an unbounded string is unbounded geometry.
constexpr size_t kMaxTitle = 48;
constexpr size_t kMaxTutorialText = 1024;

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
            // Formatted here rather than by luaL_error: Lua's own formatter
            // has no %g, and handing it one aborts with a complaint about the
            // format string instead of reporting the level's actual mistake.
            char range[64];
            std::snprintf(range, sizeof(range), "%g and %g", low, high);
            luaL_error(state, "loadout %s must be between %s", what, range);
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
    timerCallbacks_.fill(LUA_NOREF);
    pickupCallbacks_.fill(LUA_NOREF);
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
    requestedTutorial_ = -1;
    requestedShields_ = -1.0f;
    requestedSelfRepair_ = -1;
    level_ = nullptr;
    if (state_) lua_close(state_);
    state_ = nullptr;
    memory_.used = 0;
    loadout_ = ScriptLoadout{};
    loadoutDeclared_ = false;
    triggers_ = sim::TriggerSet{};
    timers_ = sim::TimerSet{};
    pickupHooks_ = sim::PickupHookSet{};
    callbacks_.fill(LUA_NOREF);
    timerCallbacks_.fill(LUA_NOREF);
    pickupCallbacks_.fill(LUA_NOREF);
    eventHooks_ = {};
    eventHookCount_ = 0;
    queryGame_ = nullptr;
    world_ = ScriptWorld{};
    messages_.clear();
    tutorials_.clear();
    callbackErrors_ = 0;
    // Emptied rather than unreffed: the registry that held these references
    // went with the state that was just closed, and there is nothing left to
    // hand them back to.
    deferred_ = {};
    elapsed_ = 0.0f;
    lastElapsed_ = 0.0f;
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
    expose(state_, this, "timer", &ScriptHost::declareTimer);
    expose(state_, this, "pickup", &ScriptHost::declarePickup);
    expose(state_, this, "comm", &ScriptHost::startComm);
    expose(state_, this, "tutorial", &ScriptHost::declareTutorial);
    expose(state_, this, "show_tutorial", &ScriptHost::showTutorial);
    expose(state_, this, "set_shields", &ScriptHost::setShields);
    expose(state_, this, "set_self_repair", &ScriptHost::setSelfRepair);
    expose(state_, this, "after", &ScriptHost::scheduleAfter);
    expose(state_, this, "event", &ScriptHost::declareEvent);

    expose(state_, this, "phase", &ScriptHost::queryPhase);
    expose(state_, this, "elapsed", &ScriptHost::queryElapsed);
    expose(state_, this, "countdown", &ScriptHost::queryCountdown);
    expose(state_, this, "reactor_alive", &ScriptHost::queryReactorAlive);
    expose(state_, this, "robots_left", &ScriptHost::queryRobotsLeft);
    expose(state_, this, "robots_alive", &ScriptHost::queryRobotsAlive);
    expose(state_, this, "has_key", &ScriptHost::queryHasKey);
    expose(state_, this, "shields", &ScriptHost::queryShields);
    expose(state_, this, "energy", &ScriptHost::queryEnergy);
    expose(state_, this, "laser_level", &ScriptHost::queryLaserLevel);
    expose(state_, this, "ammo", &ScriptHost::queryAmmo);

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

    log::info("Lua script '{}': {} trigger(s), {} timer(s), {} pickup hook(s), {} event hook(s), "
              "{} message(s), {} tutorial(s)",
              level.scriptPath, triggers_.count, timers_.count, pickupHooks_.count,
              eventHookCount_, messages_.size(), tutorials_.size());
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

int ScriptHost::declareTutorial(lua_State* state) {
    ScriptHost* host = self(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const char* id = requiredString(state, 1, "id");
    const char* clip = requiredString(state, 1, "clip");
    const char* title = requiredString(state, 1, "title");
    const char* text = requiredString(state, 1, "text");

    // %d and a cast, not %zu: Lua's own formatter understands a short fixed
    // set of specifiers and aborts on anything else, complaining about the
    // format string rather than reporting the level's actual mistake. See the
    // same trap avoided in optionalNumber above, for %g.
    if (std::strlen(id) == 0 || std::strlen(id) > kMaxMessageId) {
        return luaL_error(state, "tutorial id must contain 1..%d characters",
                          static_cast<int>(kMaxMessageId));
    }
    if (std::strlen(clip) == 0 || std::strlen(clip) > kMaxClipName) {
        return luaL_error(state, "clip must contain 1..%d characters",
                          static_cast<int>(kMaxClipName));
    }
    if (std::strlen(title) == 0 || std::strlen(title) > kMaxTitle) {
        return luaL_error(state, "tutorial title must contain 1..%d characters",
                          static_cast<int>(kMaxTitle));
    }
    if (std::strlen(text) > kMaxTutorialText) {
        return luaL_error(state, "tutorial text is longer than %d characters",
                          static_cast<int>(kMaxTutorialText));
    }
    if (host->findTutorial(id) >= 0) return luaL_error(state, "duplicate tutorial id '%s'", id);

    host->tutorials_.push_back({id, clip, title, text});
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
    if (optionalNumber(state, 1, "railgun", "railgun ammo", 0.0, 9'999.0, value)) {
        host->loadout_.railgunAmmo = static_cast<int>(value);
    }
    if (optionalNumber(state, 1, "lightning", "static charge", 0.0,
                       static_cast<double>(sim::kStaticChargeCapacity), value)) {
        host->loadout_.lightningAmmo = static_cast<int>(value);
    }
    if (optionalNumber(state, 1, "flares", "flare count", 0.0, 999.0, value)) {
        host->loadout_.flares = static_cast<int>(value);
    }
    lua_getfield(state, 1, "self_repair");
    if (!lua_isnil(state, -1)) {
        luaL_checktype(state, -1, LUA_TBOOLEAN);
        host->loadout_.selfRepair = lua_toboolean(state, -1) ? 1 : 0;
    }
    lua_pop(state, 1);

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

int ScriptHost::declareTimer(lua_State* state) {
    ScriptHost* host = self(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    if (host->timers_.count >= sim::kMaxTimers) {
        return luaL_error(state, "level exceeds the limit of %d timers", sim::kMaxTimers);
    }

    sim::TimerEvent timer;
    lua_getfield(state, 1, "at");
    timer.at = static_cast<float>(luaL_checknumber(state, -1));
    lua_pop(state, 1);
    // The floor is not fussiness. A timer at zero would be due on the level's
    // very first step, before the player has seen a frame, and a period of
    // zero would make a repeating timer due again on every step forever.
    if (!std::isfinite(timer.at) || timer.at < 0.05f || timer.at > 3600.0f) {
        return luaL_error(state, "timer at must be finite and between 0.05 and 3600 seconds");
    }

    lua_getfield(state, 1, "once");
    if (!lua_isnil(state, -1)) timer.once = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);

    lua_getfield(state, 1, "on_fire");
    if (!lua_isfunction(state, -1)) return luaL_error(state, "timer on_fire must be a function");
    const int callback = luaL_ref(state, LUA_REGISTRYINDEX);

    const int index = host->timers_.count++;
    host->timers_.items[static_cast<size_t>(index)] = timer;
    host->timerCallbacks_[static_cast<size_t>(index)] = callback;
    return 0;
}

// A response to the player collecting something. `kind` names one of the
// pickups a level can hold - the same names a level file uses, so what the
// editor wrote is what the script matches on - or "any" for all of them.
//
// The callback is handed the kind that raised it as a string. A hook on one
// kind already knows, but an "any" hook is the reason the argument exists, and
// passing it always beats two callback shapes to remember.
int ScriptHost::declarePickup(lua_State* state) {
    ScriptHost* host = self(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    if (host->pickupHooks_.count >= sim::kMaxPickupHooks) {
        return luaL_error(state, "level exceeds the limit of %d pickup hooks",
                          sim::kMaxPickupHooks);
    }

    sim::PickupHook hook;
    const char* kind = requiredString(state, 1, "kind");
    if (std::strcmp(kind, "any") != 0) {
        int matched = -1;
        for (size_t k = 0; k < kPickupNames.size(); ++k) {
            if (std::strcmp(kind, kPickupNames[k]) == 0) matched = static_cast<int>(k);
        }
        if (std::strcmp(kind, kLegacyPurplePickupName) == 0) {
            matched = static_cast<int>(PickupKind::KeyPurple);
        }
        if (matched < 0) return luaL_error(state, "unknown pickup kind '%s'", kind);
        hook.kind = matched;

        // A level that holds none of what the hook is waiting for is not a
        // broken script - a key can be moved out of a mine while the script
        // that talks about it is still being edited - but it is always a
        // mistake worth hearing about, because nothing else about it will
        // ever look wrong.
        bool present = false;
        for (const Pickup& pickup : host->level_->pickups) {
            present |= static_cast<int>(pickup.kind) == hook.kind;
        }
        if (!present) {
            log::warn("Lua pickup hook on '{}' will never fire: '{}' holds no such pickup", kind,
                      host->level_->name);
        }
    }

    // Unlike a trigger, whose volume the player can leave and fly back into, a
    // hook that is not `once` is answering a different pickup every time: the
    // second shield orb, the third box of flares. Both readings are useful, so
    // the default matches trigger and timer rather than inventing a third
    // answer to the same word.
    lua_getfield(state, 1, "once");
    if (!lua_isnil(state, -1)) hook.once = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);

    lua_getfield(state, 1, "on_pickup");
    if (!lua_isfunction(state, -1)) {
        return luaL_error(state, "pickup on_pickup must be a function");
    }
    const int callback = luaL_ref(state, LUA_REGISTRYINDEX);

    const int index = host->pickupHooks_.count++;
    host->pickupHooks_.items[static_cast<size_t>(index)] = hook;
    host->pickupCallbacks_[static_cast<size_t>(index)] = callback;
    return 0;
}

// A response to one of the moments the run itself turns on: the reactor going
// up, and the two ways a level ends. Unlike a trigger or a pickup hook there
// is nothing to filter on beyond the name, because the mine only has three of
// these and a level does not author them - it is told about them.
//
// No `once`: the sim records each moment as fired the step it happens and
// never raises it twice in a run, so every hook on it is a one-shot already.
// More than one hook may name the same moment, and all of them run.
int ScriptHost::declareEvent(lua_State* state) {
    ScriptHost* host = self(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    if (host->eventHookCount_ >= kMaxEventHooks) {
        return luaL_error(state, "level exceeds the limit of %d event hooks", kMaxEventHooks);
    }

    const char* kind = requiredString(state, 1, "kind");
    int matched = -1;
    for (size_t k = 0; k < sim::kLevelEventNames.size(); ++k) {
        if (std::strcmp(kind, sim::kLevelEventNames[k]) == 0) matched = static_cast<int>(k);
    }
    if (matched < 0) return luaL_error(state, "unknown event kind '%s'", kind);

    lua_getfield(state, 1, "on_event");
    if (!lua_isfunction(state, -1)) return luaL_error(state, "event on_event must be a function");
    const int callback = luaL_ref(state, LUA_REGISTRYINDEX);

    EventHook& hook = host->eventHooks_[static_cast<size_t>(host->eventHookCount_++)];
    hook.kind = static_cast<sim::LevelEvent>(matched);
    hook.callback = callback;
    return 0;
}

// --- queries ---------------------------------------------------------------

const sim::GameState& ScriptHost::queryGame(lua_State* state, const char* name) const {
    if (!draining_ || !queryGame_) {
        luaL_error(state, "%s() is only valid inside a callback", name);
    }
    return *queryGame_;
}

const sim::PlayerState& ScriptHost::queryPlayer(lua_State* state, const char* name) const {
    queryGame(state, name);
    if (!world_.combat) luaL_error(state, "%s() has no player to read here", name);
    return world_.combat->player;
}

const sim::RobotState& ScriptHost::queryRobots(lua_State* state, const char* name) const {
    queryGame(state, name);
    if (!world_.robots) luaL_error(state, "%s() has no robots to read here", name);
    return *world_.robots;
}

int ScriptHost::queryPhase(lua_State* state) {
    const sim::GameState& game = self(state)->queryGame(state, "phase");
    const size_t i = static_cast<size_t>(game.phase);
    lua_pushstring(state, i < sim::kGamePhaseNames.size() ? sim::kGamePhaseNames[i] : "playing");
    return 1;
}

int ScriptHost::queryElapsed(lua_State* state) {
    lua_pushnumber(state, self(state)->queryGame(state, "elapsed").elapsed);
    return 1;
}

// Seconds left to get out, or -1 while the reactor is still standing. A
// negative answer rather than nil so that arithmetic on it never blows up in
// the middle of somebody's countdown line.
int ScriptHost::queryCountdown(lua_State* state) {
    const sim::GameState& game = self(state)->queryGame(state, "countdown");
    lua_pushnumber(state,
                   game.phase == sim::GamePhase::Countdown ? game.countdown : -1.0f);
    return 1;
}

int ScriptHost::queryReactorAlive(lua_State* state) {
    lua_pushboolean(state, self(state)->queryGame(state, "reactor_alive").reactorAlive ? 1 : 0);
    return 1;
}

int ScriptHost::queryRobotsLeft(lua_State* state) {
    const sim::RobotState& robots = self(state)->queryRobots(state, "robots_left");
    lua_pushinteger(state, robots.aliveCount());
    return 1;
}

// How many of one type are still flying. Matching on type and not on identity
// is deliberate: a robot's index is its position in the level file among the
// objects that happen to be robots, which nothing about the format promises
// to keep stable. A type is named in the file and means the same thing after
// somebody reorders it.
int ScriptHost::queryRobotsAlive(lua_State* state) {
    ScriptHost* host = self(state);
    const char* type = luaL_checkstring(state, 1);
    const sim::RobotState& robots = host->queryRobots(state, "robots_alive");
    if (!host->world_.robotConfig) {
        return luaL_error(state, "robots_alive() has no robot types to read here");
    }
    const int wanted = host->world_.robotConfig->indexOf(type);
    if (wanted < 0) return luaL_error(state, "unknown robot type '%s'", type);

    int alive = 0;
    for (int i = 0; i < robots.count; ++i) {
        const sim::Robot& robot = robots.robots[static_cast<size_t>(i)];
        if (robot.alive && robot.type == wanted) ++alive;
    }
    lua_pushinteger(state, alive);
    return 1;
}

int ScriptHost::queryHasKey(lua_State* state) {
    const char* color = luaL_checkstring(state, 1);
    if (std::strcmp(color, kLegacyPurpleKeyName) == 0) {
        color = kKeyNames[static_cast<size_t>(KeyColor::Purple)];
    }
    const sim::GameState& game = self(state)->queryGame(state, "has_key");
    for (size_t i = 0; i < kKeyNames.size(); ++i) {
        if (std::strcmp(color, kKeyNames[i]) != 0) continue;
        lua_pushboolean(state, i < game.keys.size() && game.keys[i] ? 1 : 0);
        return 1;
    }
    return luaL_error(state, "unknown key colour '%s'", color);
}

int ScriptHost::queryShields(lua_State* state) {
    lua_pushnumber(state, self(state)->queryPlayer(state, "shields").shields);
    return 1;
}

int ScriptHost::queryEnergy(lua_State* state) {
    lua_pushnumber(state, self(state)->queryPlayer(state, "energy").energy);
    return 1;
}

int ScriptHost::queryLaserLevel(lua_State* state) {
    lua_pushinteger(state, self(state)->queryPlayer(state, "laser_level").laserLevel);
    return 1;
}

// Named with the words loadout{} uses, so the thing a level starts the ship
// with and the thing a script asks about later are spelled the same way.
int ScriptHost::queryAmmo(lua_State* state) {
    const char* kind = luaL_checkstring(state, 1);
    const sim::PlayerState& player = self(state)->queryPlayer(state, "ammo");
    if (std::strcmp(kind, "vulcan") == 0) lua_pushinteger(state, player.vulcanAmmo);
    else if (std::strcmp(kind, "concussion") == 0) lua_pushinteger(state, player.concussionAmmo);
    else if (std::strcmp(kind, "homing") == 0) lua_pushinteger(state, player.homingAmmo);
    else if (std::strcmp(kind, "railgun") == 0) lua_pushinteger(state, player.railgunAmmo);
    else if (std::strcmp(kind, "lightning") == 0) lua_pushinteger(state, player.lightningAmmo);
    else if (std::strcmp(kind, "flares") == 0) lua_pushinteger(state, player.flares);
    else return luaL_error(state, "unknown ammunition '%s'", kind);
    return 1;
}

// Hands the ship back, or takes it away. The one thing a script can change
// about the player mid-level, and the reason the intro can start the ship
// disabled and repair it on a cue.
//
// Latched rather than written, for the same reason comm() is: Lua runs
// outside the fixed step and must not reach into simulation state. Whoever
// drains the callbacks reads this and applies it. See takeShields().
int ScriptHost::setShields(lua_State* state) {
    ScriptHost* host = self(state);
    const double value = luaL_checknumber(state, 1);
    if (!host->draining_) return luaL_error(state, "set_shields() is only valid inside a callback");
    if (!std::isfinite(value) || value < 0.0 ||
        value > static_cast<double>(sim::kPlayerMaxShields)) {
        return luaL_error(state, "set_shields expects a value between zero and full shields");
    }

    host->requestedShields_ = static_cast<float>(value);
    return 0;
}

// Lets a disabled ship repair itself again, or stops it. Latched like
// set_shields(), and for the same reason.
int ScriptHost::setSelfRepair(lua_State* state) {
    ScriptHost* host = self(state);
    luaL_checktype(state, 1, LUA_TBOOLEAN);
    if (!host->draining_) {
        return luaL_error(state, "set_self_repair() is only valid inside a callback");
    }
    host->requestedSelfRepair_ = lua_toboolean(state, 1) ? 1 : 0;
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

// The modal counterpart of comm(). Latched for the same reason: Lua runs
// outside the fixed step, and what this asks for stops the simulation and
// takes the mouse, which is the game layer's to do and not a script's.
int ScriptHost::showTutorial(lua_State* state) {
    ScriptHost* host = self(state);
    const char* id = luaL_checkstring(state, 1);
    if (!host->draining_) {
        return luaL_error(state, "show_tutorial() is only valid inside a callback");
    }
    const int index = host->findTutorial(id);
    if (index < 0) return luaL_error(state, "unknown tutorial id '%s'", id);
    // First one wins. A second pane in the same drain would replace one the
    // player has not been shown yet, and silently losing authored content is
    // worse than saying so.
    if (host->requestedTutorial_ >= 0) {
        log::warn("Lua show_tutorial('{}') ignored: a tutorial was already raised this frame",
                  id);
        return 0;
    }

    host->requestedTutorial_ = index;
    return 0;
}

// The one thing a callback can do that happens later: after(15, function() ...
// end) puts a second beat fifteen seconds behind the first, which is what a
// comm on a key followed by a tutorial about what the key opens actually
// wants. A timer cannot express it, because a timer's moment is authored
// against the level's start and this one is not known until the player does
// something.
//
// Counted on the level clock, so a delay does not run down while the game is
// paused or while a tutorial pane holds the simulation. Reference-counted in
// the registry exactly like an authored callback, and dropped wholesale by a
// restart: see cancelDeferred().
int ScriptHost::scheduleAfter(lua_State* state) {
    ScriptHost* host = self(state);
    const double seconds = luaL_checknumber(state, 1);
    luaL_checktype(state, 2, LUA_TFUNCTION);
    if (!host->draining_) return luaL_error(state, "after() is only valid inside a callback");
    // The same floor a timer has, and for the same reason: a delay of zero is
    // not a delay, and something that wants to happen now can just happen now.
    if (!std::isfinite(seconds) || seconds < 0.05 || seconds > 3600.0) {
        return luaL_error(state, "after() expects a delay between 0.05 and 3600 seconds");
    }

    int slot = -1;
    for (int i = 0; i < kMaxDeferredCalls && slot < 0; ++i) {
        if (!host->deferred_[static_cast<size_t>(i)].active) slot = i;
    }
    // Warned rather than raised, like a second tutorial in one frame. An
    // error here would disable the callback that made it, which is a far
    // bigger loss than the one beat that could not be scheduled.
    if (slot < 0) {
        log::warn("Lua after({:.2f}) ignored: {} delayed calls are already outstanding", seconds,
                  kMaxDeferredCalls);
        return 0;
    }

    lua_pushvalue(state, 2);
    Deferred& entry = host->deferred_[static_cast<size_t>(slot)];
    entry.callback = luaL_ref(state, LUA_REGISTRYINDEX);
    entry.due = host->elapsed_ + static_cast<float>(seconds);
    entry.active = true;
    return 0;
}

void ScriptHost::cancelDeferred() {
    for (Deferred& entry : deferred_) {
        if (!entry.active) continue;
        if (state_) luaL_unref(state_, LUA_REGISTRYINDEX, entry.callback);
        entry = Deferred{};
    }
}

// Every delay that has come due, in slot order. The slot is emptied before the
// call rather than after it, so a callback that schedules another delay can
// have the slot it just vacated - and so a callback that fails is already off
// the list by the time it is disabled.
//
// Nothing scheduled during this pass can run in it: after() counts from
// elapsed_, which is fixed for the whole drain, and its floor is well clear of
// zero.
void ScriptHost::runDeferred() {
    for (Deferred& entry : deferred_) {
        if (!entry.active || entry.due > elapsed_) continue;

        const int callback = entry.callback;
        entry = Deferred{};
        call(callback, "delayed callback");
        luaL_unref(state_, LUA_REGISTRYINDEX, callback);
    }
}

int ScriptHost::findMessage(const char* id) const {
    for (size_t i = 0; i < messages_.size(); ++i) {
        if (messages_[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

int ScriptHost::findTutorial(const char* id) const {
    for (size_t i = 0; i < tutorials_.size(); ++i) {
        if (tutorials_[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

bool ScriptHost::call(int functionRef, const char* context, const char* argument) {
    lua_rawgeti(state_, LUA_REGISTRYINDEX, functionRef);
    const int args = argument ? 1 : 0;
    if (argument) lua_pushstring(state_, argument);
    lua_sethook(state_, &ScriptHost::instructionLimit, LUA_MASKCOUNT, kInstructionBudget);
    const int status = lua_pcall(state_, args, 0, 0);
    lua_sethook(state_, nullptr, 0, 0);
    if (status == LUA_OK) return true;

    const char* error = lua_tostring(state_, -1);
    log::error("Lua {} failed and has been disabled: {}", context,
               error ? error : "unknown error");
    ++callbackErrors_;
    lua_pop(state_, 1);
    return false;
}

void ScriptHost::drain(sim::GameState& game, const ScriptWorld& world) {
    if (!state_) {
        game.triggers.clearPending();
        game.timers.clearPending();
        game.pickupHooks.clearPending();
        game.levelEvents.clearPending();
        return;
    }

    // What the queries read, for the length of this drain and no longer. A
    // query outside one has nothing to answer about, and a pointer kept past
    // it would be a pointer into a frame that has moved on.
    queryGame_ = &game;
    world_ = world;

    // A clock that has gone backwards is a level that has started again, and
    // a delay scheduled by the run before it belongs to nobody. Noticed here
    // rather than announced by whoever restarts, because there is more than
    // one way back to the beginning of a level and a hook every one of them
    // has to remember to call is a hook one of them will not.
    if (game.elapsed < lastElapsed_) cancelDeferred();
    lastElapsed_ = game.elapsed;
    elapsed_ = game.elapsed;

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

    // Pickups after volumes and before timers: within one frame, what the
    // player flew into, then what the player took, then what merely came
    // round on the clock. The two things the player did are heard from in the
    // order the mine put them in front of them.
    const int pickupCount = game.pickupHooks.pendingCount;
    for (int i = 0; i < pickupCount; ++i) {
        const sim::PickupFire& fire = game.pickupHooks.pending[static_cast<size_t>(i)];
        int& callback = pickupCallbacks_[static_cast<size_t>(fire.hook)];
        if (callback == LUA_NOREF) continue;
        // Always a real kind, since only a collected pickup queues one; the
        // bound is here because the index arrives as a byte out of sim state.
        const size_t kind = fire.kind;
        if (!call(callback, "pickup callback",
                  kind < kPickupNames.size() ? kPickupNames[kind] : "unknown")) {
            luaL_unref(state_, LUA_REGISTRYINDEX, callback);
            callback = LUA_NOREF;
        }
    }

    // The run's own moments after the two things the player did, because that
    // is the order they happen in: the missile goes into the reactor, then the
    // reactor goes up. Every hook on a moment runs; there is no first-one-wins
    // here, because a level may reasonably want two things said at once.
    const int eventCount = game.levelEvents.pendingCount;
    for (int i = 0; i < eventCount; ++i) {
        const size_t event = game.levelEvents.pending[static_cast<size_t>(i)];
        for (int h = 0; h < eventHookCount_; ++h) {
            EventHook& hook = eventHooks_[static_cast<size_t>(h)];
            if (hook.callback == LUA_NOREF) continue;
            if (static_cast<size_t>(hook.kind) != event) continue;
            if (!call(hook.callback, "event callback",
                      event < sim::kLevelEventNames.size() ? sim::kLevelEventNames[event]
                                                           : "unknown")) {
                luaL_unref(state_, LUA_REGISTRYINDEX, hook.callback);
                hook.callback = LUA_NOREF;
            }
        }
    }

    // Timers after volumes, so that within one frame a place the player has
    // just flown into is heard from before a moment that merely came round.
    const int timerCount = game.timers.pendingCount;
    for (int i = 0; i < timerCount; ++i) {
        const int timer = game.timers.pending[static_cast<size_t>(i)];
        int& callback = timerCallbacks_[static_cast<size_t>(timer)];
        if (callback == LUA_NOREF) continue;
        if (!call(callback, "timer callback")) {
            luaL_unref(state_, LUA_REGISTRYINDEX, callback);
            callback = LUA_NOREF;
        }
    }
    // Delays last, after everything this frame has had its say: a beat put in
    // the air fifteen seconds ago is owed less immediacy than the wall the
    // player has just flown into.
    runDeferred();
    draining_ = false;
    queryGame_ = nullptr;
    world_ = ScriptWorld{};
    game.triggers.clearPending();
    game.timers.clearPending();
    game.pickupHooks.clearPending();
    game.levelEvents.clearPending();
}

const ScriptMessage* ScriptHost::message(int index) const {
    if (index < 0 || static_cast<size_t>(index) >= messages_.size()) return nullptr;
    return &messages_[static_cast<size_t>(index)];
}

const ScriptTutorial* ScriptHost::tutorial(int index) const {
    if (index < 0 || static_cast<size_t>(index) >= tutorials_.size()) return nullptr;
    return &tutorials_[static_cast<size_t>(index)];
}

int ScriptHost::takeMessage() {
    const int message = requestedMessage_;
    requestedMessage_ = -1;
    return message;
}

int ScriptHost::takeTutorial() {
    const int tutorial = requestedTutorial_;
    requestedTutorial_ = -1;
    return tutorial;
}

float ScriptHost::takeShields() {
    const float shields = requestedShields_;
    requestedShields_ = -1.0f;
    return shields;
}

int ScriptHost::takeSelfRepair() {
    const int selfRepair = requestedSelfRepair_;
    requestedSelfRepair_ = -1;
    return selfRepair;
}

// What the frame loop does after every drain, and the reason it matters here:
// the latches are read once and cleared, so a test that never reads them makes
// one frame's request look like it is still outstanding on the next. A second
// tutorial would then be refused as a duplicate of a pane that was shown and
// finished with several triggers ago.
static void drainRequests(ScriptHost& host) {
    host.takeMessage();
    host.takeTutorial();
    host.takeShields();
    host.takeSelfRepair();
}

static int runOneScriptTest(const std::string& levelPath) {
    Level level = loadLevel(levelPath);
    ScriptHost host;
    host.load(level);
    if (!host.loaded()) {
        log::info("script-test: '{}' has no script: passed", levelPath);
        return 0;
    }

    // A mine for the queries to read. Empty - no robots, a default ship - but
    // real, so a script that asks something inside a callback is answered
    // rather than refused and counted as a broken callback.
    sim::CombatState combat;
    sim::RobotState robots;
    const sim::RobotConfig robotConfig = sim::RobotConfig::defaults();
    const ScriptWorld world{&combat, &robots, &robotConfig};

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
        host.drain(game, world);
        drainRequests(host);

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
        host.drain(game, world);
        drainRequests(host);
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
    if (loadout.railgunAmmo >= 0) player.railgunAmmo = loadout.railgunAmmo;
    if (loadout.lightningAmmo >= 0) player.lightningAmmo = loadout.lightningAmmo;
    if (loadout.flares >= 0) player.flares = loadout.flares;
    if (loadout.selfRepair >= 0) player.selfRepair = loadout.selfRepair != 0;
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
