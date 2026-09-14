#include "sim/gameflow.h"

#include "core/log.h"
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

void setBlocked(GameState& state, const Level& level, size_t doorIndex, bool blocked) {
    const Door& door = level.doors[doorIndex];
    const auto mark = [&](int segment, int side) {
        const size_t i = static_cast<size_t>(segment) * SideCount + static_cast<size_t>(side);
        if (i < state.doorField.blocked.size()) {
            state.doorField.blocked[i] = blocked ? 1u : 0u;
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

void initGame(const Level& level, const RobotState& robots, GameState& out) {
    out = GameState{};
    out.robotsAtStart = robots.aliveCount();
    out.reactorAlive = false;
    for (size_t i = 0; i < level.doors.size() && i < kMaxDoors; ++i) {
        setBlocked(out, level, i, true);
    }
}

void stepGame(const Level& level, const GameConfig& cfg, GameState& state, CombatState& combat,
              RobotState& robots, const RobotConfig& robotCfg, Body& player, float dt) {
    if (state.phase == GamePhase::Won || state.phase == GamePhase::Lost) return;

    state.elapsed += dt;

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
        const bool mayOpen = hasKey(state, door.key);

        if (someoneNear && mayOpen) {
            ds.wantOpen = true;
            ds.holdTimer = door.closeDelay;
        } else if (ds.holdTimer > 0.0f) {
            ds.holdTimer -= dt;
        } else {
            ds.wantOpen = false;
        }

        const float rate = door.openTime > 0.0f ? dt / door.openTime : 1.0f;
        ds.openness = std::clamp(ds.openness + (ds.wantOpen ? rate : -rate), 0.0f, 1.0f);
        if (ds.openness > 0.0f) ds.everOpened = true;

        setBlocked(state, level, di, ds.openness < kPassableAt);
    }

    // --- pickups ---------------------------------------------------------
    for (size_t pi = 0; pi < level.pickups.size() && pi < kMaxPickups; ++pi) {
        if (state.collected[pi]) continue;
        const Pickup& pickup = level.pickups[pi];
        if (glm::length(player.position - pickup.position) > player.radius + 2.4f) continue;

        state.collected[pi] = true;
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
    }

    // --- the reactor -----------------------------------------------------
    bool reactorFound = false;
    for (int ri = 0; ri < robots.count; ++ri) {
        const Robot& robot = robots.robots[static_cast<size_t>(ri)];
        const RobotDef& def = robotCfg.types[static_cast<size_t>(robot.type)];
        if (!def.isReactor) continue;
        reactorFound = true;
        if (robot.alive) state.reactorAlive = true;
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
            log::info("reactor destroyed: {:.0f} seconds to get out", state.countdown);
        }
    }

    if (state.phase == GamePhase::Countdown) {
        state.countdown -= dt;
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
            log::info("escaped with {:.1f}s to spare, {} robots destroyed", state.countdown,
                      state.robotsDestroyed);
            return;
        }

        if (state.countdown <= 0.0f) {
            state.phase = GamePhase::Lost;
            state.endReason = "the mine went up with you still in it";
            log::info("countdown reached zero");
            return;
        }
    }

    if (combat.player.shields <= 0.0f) {
        state.phase = GamePhase::Lost;
        state.endReason = "shields gone";
        log::info("destroyed after {:.1f}s, {} robots destroyed", state.elapsed,
                  state.robotsDestroyed);
    }
}

} // namespace dvk::sim
