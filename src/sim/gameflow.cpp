#include "sim/gameflow.h"

#include "core/log.h"
#include "sim/collision.h"
#include "sim/combat.h"
#include "sim/robots.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace dvk::sim {

namespace {

// A door counts as passable a little before it is fully open, so flying
// through one does not feel like squeezing past it.
constexpr float kPassableAt = 0.75f;

void setBlocked(GameState& state, const Level& level, size_t doorIndex, bool blocked,
                bool opaque) {
    const Door& door = level.doors[doorIndex];
    const auto mark = [&](int segment, int side) {
        const size_t i = static_cast<size_t>(segment) * SideCount + static_cast<size_t>(side);
        if (i < state.doorField.blocked.size()) {
            state.doorField.blocked[i] = blocked ? 1u : 0u;
            state.doorField.opaque[i] = opaque ? 1u : 0u;
        }
    };
    mark(door.segment, door.side);
    // And from the other side, so a body or a light coming the other way sees
    // the same closed door.
    const Segment& other = level.segments[static_cast<size_t>(door.neighbour)];
    for (int os = 0; os < SideCount; ++os) {
        if (other.sides[static_cast<size_t>(os)].neighbour == door.segment) {
            mark(door.neighbour, os);
            break;
        }
    }
}

bool hasKey(const GameState& state, KeyColor key) {
    if (key == KeyColor::None) return true;
    const auto i = static_cast<size_t>(key);
    return i < state.keys.size() && state.keys[i];
}

} // namespace

bool sidePassable(const GameState& state, int segment, int side) {
    return !state.doorField.isBlocked(segment, side);
}

void buildSolidDoors(const Level& level, const GameState& state, SolidDoors& out) {
    out.clear();
    for (size_t di = 0; di < level.doors.size() && di < kMaxDoors; ++di) {
        if (state.doors[di].openness >= kPassableAt) continue;
        const Door& door = level.doors[di];
        const int split[2][3] = {{0, 1, 2}, {0, 2, 3}};
        for (int t = 0; t < 2 && out.count < SolidDoors::kCapacity; ++t) {
            SolidDoors::Face& face = out.faces[static_cast<size_t>(out.count++)];
            for (int k = 0; k < 3; ++k) {
                face.p[static_cast<size_t>(k)] = door.corners[static_cast<size_t>(split[t][k])];
            }
            face.normal = door.normal;
            face.segmentA = door.segment;
            face.segmentB = door.neighbour;
            face.opaque = state.doors[di].openness <= 0.0f;
        }
    }
}

void initGame(const Level& level, const RobotState& robots, GameState& out) {
    out = GameState{};
    out.robotsAtStart = robots.aliveCount();
    out.reactorAlive = false;
    for (size_t i = 0; i < level.doors.size() && i < kMaxDoors; ++i) {
        setBlocked(out, level, i, true, true);
    }
}

void stepGame(const Level& level, const GameConfig& cfg, GameState& state, CombatState& combat,
              RobotState& robots, const RobotConfig& robotCfg, Body& player, float dt,
              EventQueue* events, const TriggerSet* triggers, const TimerSet* timers,
              const PickupHookSet* pickupHooks) {
    if (state.phase == GamePhase::Won || state.phase == GamePhase::Lost) return;

    state.elapsed += dt;

    stepTriggers(state.triggers, player, triggers);
    // After the clock has been advanced, so a timer authored at zero fires on
    // the level's first step rather than one step late.
    stepTimers(state.timers, state.elapsed, timers);

    // --- what the player has found ---------------------------------------
    //
    // Purely a record for the automap; nothing below reads it back.
    if (player.segment >= 0 && player.segment < kMaxFlowSegments &&
        !state.visited[static_cast<size_t>(player.segment)]) {
        state.visited[static_cast<size_t>(player.segment)] = true;
        ++state.visitedCount;

        // A pickup is on the map once the player has been in the room with it,
        // which means a key noticed and left behind can be flown back to.
        for (size_t pi = 0; pi < level.pickups.size() && pi < kMaxPickups; ++pi) {
            if (level.pickups[pi].segment == player.segment) state.seen[pi] = true;
        }
    }

    // --- doors -----------------------------------------------------------
    for (size_t di = 0; di < level.doors.size() && di < kMaxDoors; ++di) {
        const Door& door = level.doors[di];
        DoorState& ds = state.doors[di];

        // Anything that can open a door: the player, or a robot. A door that
        // only the player can open makes a mine full of robots feel like a
        // museum.
        bool someoneNear = glm::length(player.position - door.center) < door.openDistance;
        if (!someoneNear) {
            for (int ri = 0; ri < robots.count && !someoneNear; ++ri) {
                const Robot& robot = robots.robots[static_cast<size_t>(ri)];
                if (!robot.alive) continue;
                someoneNear = glm::length(robot.body.position - door.center) < door.openDistance;
            }
        }

        // A locked door does not even acknowledge you without the key. Robots
        // carry no keys, so a locked door stays locked to them too.
        //
        // An emergency hatch answers to the reactor rather than to anything
        // carried, and `exitOpen` is already the moment the reactor went up.
        // Robots cannot open one either, which is the point: the way out is
        // shut until the player has earned it.
        const bool mayOpen =
            hasKey(state, door.key) && (!door.requiresReactor || state.exitOpen);

        ds.lockedTimer = std::max(0.0f, ds.lockedTimer - dt);
        // Refused. Only for the player: a robot bumping into a door it cannot
        // open is not something the player needs told about, and every robot in
        // the mine buzzing at every locked door would be unbearable.
        if (!mayOpen && ds.lockedTimer <= 0.0f &&
            glm::length(player.position - door.center) < door.openDistance) {
            ds.lockedTimer = kLockedRefusalInterval;
            emit(events, Cue::DoorLocked, door.center, door.segment);
        }

        const bool wasOpening = ds.wantOpen;
        if (someoneNear && mayOpen) {
            ds.wantOpen = true;
            ds.holdTimer = door.closeDelay;
        } else if (ds.holdTimer > 0.0f) {
            ds.holdTimer -= dt;
        } else {
            ds.wantOpen = false;
        }

        // The slab starting to move is the sound, not the slab arriving. A door
        // already at one end of its travel is not starting anything.
        if (ds.wantOpen && !wasOpening && ds.openness < 1.0f) {
            emit(events, Cue::DoorOpen, door.center, door.segment);
        } else if (!ds.wantOpen && wasOpening && ds.openness > 0.0f) {
            emit(events, Cue::DoorClose, door.center, door.segment);
        }

        const float rate = door.openTime > 0.0f ? dt / door.openTime : 1.0f;
        ds.openness = std::clamp(ds.openness + (ds.wantOpen ? rate : -rate), 0.0f, 1.0f);
        if (ds.openness > 0.0f) ds.everOpened = true;

        // Passable a little before fully open; see-through as soon as it has
        // cracked open at all.
        setBlocked(state, level, di, ds.openness < kPassableAt, ds.openness <= 0.0f);
    }

    // --- pickups ---------------------------------------------------------
    for (size_t pi = 0; pi < level.pickups.size() && pi < kMaxPickups; ++pi) {
        if (state.collected[pi]) continue;
        const Pickup& pickup = level.pickups[pi];
        if (glm::length(player.position - pickup.position) > player.radius + 2.4f) continue;

        state.collected[pi] = true;
        // A key is the one pickup that changes where the player can go, so it
        // gets its own, more emphatic noise.
        const bool isKey = pickup.kind == PickupKind::KeyBlue ||
                           pickup.kind == PickupKind::KeyYellow ||
                           pickup.kind == PickupKind::KeyRed;
        emit(events, isKey ? Cue::KeyPickup : Cue::Pickup, pickup.position, pickup.segment);
        PlayerState& p = combat.player;
        switch (pickup.kind) {
            case PickupKind::Shield:
                p.shields = std::min(100.0f, p.shields + cfg.shieldPickup);
                break;
            case PickupKind::Energy:
                p.energy = std::min(100.0f, p.energy + cfg.energyPickup);
                break;
            case PickupKind::LaserUpgrade:
                p.laserLevel = std::min(kLaserLevels, p.laserLevel + 1);
                break;
            case PickupKind::Concussion:
                p.concussionAmmo += cfg.concussionPickup;
                break;
            case PickupKind::Homing:
                p.homingAmmo += cfg.homingPickup;
                break;
            case PickupKind::RailgunAmmo:
                p.railgunAmmo += cfg.railgunPickup;
                break;
            case PickupKind::LightningAmmo:
                p.lightningAmmo += cfg.lightningPickup;
                break;
            case PickupKind::Flares:
                p.flares += cfg.flarePickup;
                break;
            case PickupKind::KeyBlue:
                state.keys[0] = true;
                break;
            case PickupKind::KeyYellow:
                state.keys[1] = true;
                break;
            case PickupKind::KeyRed:
                state.keys[2] = true;
                break;
            case PickupKind::Count:
                break;
        }

        // After the effect, not before it: a callback raised from here runs
        // once the frame's steps are done, and what it says about the thing
        // just collected should be true by the time it says it.
        notePickup(state.pickupHooks, static_cast<int>(pickup.kind), pickupHooks);
    }

    // --- the reactor -----------------------------------------------------
    bool reactorFound = false;
    for (int ri = 0; ri < robots.count; ++ri) {
        const Robot& robot = robots.robots[static_cast<size_t>(ri)];
        const RobotDef& def = robotCfg.types[static_cast<size_t>(robot.type)];
        if (!def.isReactor) continue;
        reactorFound = true;
        if (robot.alive) state.reactorAlive = true;

        // The map marks the reactor once the player has actually laid eyes on
        // it, which is a stronger claim than "has been in its room" and wants a
        // real line of sight. Tested with the same query the robots use, against
        // the doors as they stand this step, so a reactor behind a shut door is
        // not revealed through it.
        //
        // Only worth doing until it succeeds once, so this is at most a handful
        // of rays per level rather than per step.
        if (!state.reactorSeen && robot.alive) {
            SolidDoors solid;
            buildSolidDoors(level, state, solid);
            const glm::vec3 toReactor = robot.body.position - player.position;
            const float distance = glm::length(toReactor);
            if (distance > 1.0e-4f) {
                const RayHit hit = raycast(level, player.position, toReactor / distance, distance,
                                           player.segment, &solid);
                if (!hit.hit || hit.distance >= distance - robot.body.radius) {
                    state.reactorSeen = true;
                    state.reactorPosition = robot.body.position;
                    log::info("reactor sighted at {:.1f}s", state.elapsed);
                }
            }
        }
        if (state.reactorSeen && robot.alive) state.reactorPosition = robot.body.position;
    }
    if (!reactorFound) state.reactorAlive = false;

    int alive = 0;
    for (int ri = 0; ri < robots.count; ++ri) {
        if (robots.robots[static_cast<size_t>(ri)].alive) ++alive;
    }
    state.robotsDestroyed = std::max(0, state.robotsAtStart - alive);

    if (state.phase == GamePhase::Playing) {
        bool reactorGone = reactorFound;
        for (int ri = 0; ri < robots.count && reactorGone; ++ri) {
            const Robot& robot = robots.robots[static_cast<size_t>(ri)];
            const RobotDef& def = robotCfg.types[static_cast<size_t>(robot.type)];
            if (def.isReactor && robot.alive) reactorGone = false;
        }
        if (reactorGone) {
            state.phase = GamePhase::Countdown;
            state.countdown = cfg.countdownSeconds;
            state.exitOpen = true;
            // At the listener: the reactor going up is the whole mine, and
            // attenuating it by how far the player had already run from the
            // room would be exactly backwards.
            emitLocal(events, Cue::ReactorDestroyed);
            log::info("reactor destroyed: {:.0f} seconds to get out", state.countdown);
        }
    }

    if (state.phase == GamePhase::Countdown) {
        const float before = state.countdown;
        state.countdown -= dt;
        // One warning per whole second crossed, from the clock itself rather
        // than from a timer of its own, so the beeps cannot drift away from the
        // number on the HUD.
        if (state.countdown > 0.0f &&
            std::floor(before) != std::floor(state.countdown)) {
            emitLocal(events, Cue::CountdownWarn);
        }
        // Shake ramps from nothing to full over the countdown, so the mine
        // coming apart is a clock the player can feel without reading it.
        const float fraction =
            cfg.countdownSeconds > 0.0f
                ? std::clamp(1.0f - state.countdown / cfg.countdownSeconds, 0.0f, 1.0f)
                : 1.0f;
        state.shake = fraction * fraction;

        if (level.exitTrigger.present &&
            glm::length(player.position - level.exitTrigger.position) <
                level.exitTrigger.radius + player.radius) {
            state.phase = GamePhase::Won;
            state.endReason = "escaped";
            emitLocal(events, Cue::Exit);
            log::info("escaped with {:.1f}s to spare, {} robots destroyed", state.countdown,
                      state.robotsDestroyed);
            return;
        }

        if (state.countdown <= 0.0f) {
            state.phase = GamePhase::Lost;
            state.endReason = "the mine went up with you still in it";
            emitLocal(events, Cue::PlayerDeath);
            log::info("countdown reached zero");
            return;
        }
    }

    if (combat.player.shields <= 0.0f) {
        // Here rather than in checkPlayerDeath: from M7 running out of shields
        // ends the level instead of respawning, and checkPlayerDeath is only
        // reached by --weapons-test. A cue emitted there alone would never fire
        // in a played game.
        if (state.phase != GamePhase::Lost) emitLocal(events, Cue::PlayerDeath);
        state.phase = GamePhase::Lost;
        state.endReason = "shields gone";
        log::info("destroyed after {:.1f}s, {} robots destroyed", state.elapsed,
                  state.robotsDestroyed);
    }
}

} // namespace dvk::sim
