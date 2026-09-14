#include "game/loop_test.h"

#include "core/log.h"
#include "game/level.h"
#include "sim/combat.h"
#include "sim/gameflow.h"
#include "sim/robots.h"
#include "sim/weapons.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace dvk {

namespace {

constexpr float kStep = 1.0f / 120.0f;

struct Rig {
    Level level;
    sim::WeaponConfig weapons;
    sim::RobotConfig robotCfg;
    sim::GameConfig gameCfg;
    sim::RobotState robots;
    sim::CombatState combat;
    sim::GameState game;
    sim::Body player;
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};

    void reset() {
        combat = sim::CombatState{};
        sim::initObjects(level, combat.objects);
        sim::spawnRobots(level, robotCfg, robots);
        sim::initGame(level, robots, game);
        player = sim::Body{};
        player.radius = 1.6f;
        player.position = level.start.position;
        player.segment = level.start.segment;
        combat.player.headlightOn = false;
    }

    // Robots are deliberately not stepped in most of these checks: this test is
    // about the level's flow, and a gunner emptying the player's shields
    // halfway through a door test would be noise.
    void run(int steps, bool withRobots = false) {
        const sim::FireControls idle;
        for (int i = 0; i < steps; ++i) {
            sim::stepCombat(level, combat, player, orientation, weapons, idle, kStep, &robots);
            if (withRobots) {
                sim::stepRobots(level, robotCfg, weapons, robots, combat, player, kStep);
            }
            sim::stepGame(level, gameCfg, game, combat, robots, robotCfg, player, kStep);
        }
    }

    void teleport(const glm::vec3& position, int segment) {
        player.position = position;
        player.segment = segment >= 0 ? segment : findSegment(level, position);
        player.velocity = glm::vec3(0.0f);
    }
};

bool expect(bool condition, const char* what) {
    if (!condition) log::error("FAILED: {}", what);
    return condition;
}

} // namespace

int runLoopTest(const char* levelPath, const char* weaponConfigPath, const char* robotConfigPath) {
    Rig rig;
    rig.level = loadLevel(levelPath);
    rig.weapons = sim::loadWeaponConfig(weaponConfigPath);
    rig.robotCfg = sim::loadRobotConfig(robotConfigPath);
    bool ok = true;

    // Only a campaign level has a loop to test. test01 is a renderer and
    // physics fixture with no doors, no exit and no reactor, and running the
    // rest of this against it indexed an empty vector and took the process
    // with it.
    if (rig.level.doors.empty() || !rig.level.exitTrigger.present) {
        log::info("'{}' is not a campaign level: {} doors, {} pickups, exit {}. Nothing to play "
                  "through - run this against mine01.",
                  rig.level.name, rig.level.doors.size(), rig.level.pickups.size(),
                  rig.level.exitTrigger.present ? "present" : "absent");
        log::info("loop test: SKIPPED");
        return 0;
    }

    ok &= expect(rig.level.doors.size() == 3, "the level has three keyed doors");
    ok &= expect(!rig.level.pickups.empty(), "the level has pickups");

    // --- each key opens its own door, and only its own ----------------------
    {
        for (size_t di = 0; di < rig.level.doors.size(); ++di) {
            const Door& door = rig.level.doors[di];
            const auto keyIndex = static_cast<size_t>(door.key);

            // Locked, standing right in front of it.
            rig.reset();
            rig.teleport(door.center - door.normal * 4.0f, door.segment);
            rig.run(240);
            const bool stayedShut = rig.game.doors[di].openness <= 0.01f;
            ok &= expect(stayedShut, "a locked door stayed shut without its key");

            // Every other key, and still nothing.
            rig.reset();
            for (size_t k = 0; k < rig.game.keys.size(); ++k) {
                rig.game.keys[k] = (k != keyIndex);
            }
            rig.teleport(door.center - door.normal * 4.0f, door.segment);
            rig.run(240);
            const bool wrongKeyFailed = rig.game.doors[di].openness <= 0.01f;
            ok &= expect(wrongKeyFailed, "the wrong keys did not open the door");

            // The right key, and it opens.
            rig.reset();
            rig.game.keys[keyIndex] = true;
            rig.teleport(door.center - door.normal * 4.0f, door.segment);
            rig.run(240);
            const bool opened = rig.game.doors[di].openness > 0.99f;
            ok &= expect(opened, "the right key opened the door");

            log::info("door {} ({}): locked {}, wrong key {}, right key {}", di,
                      kKeyNames[keyIndex], stayedShut ? "held" : "OPENED",
                      wrongKeyFailed ? "held" : "OPENED", opened ? "opened" : "STUCK");
        }
    }

    // --- a closed door blocks light -----------------------------------------
    //
    // The same DoorField the portal walk and collision use also stops the
    // light flood, so this is a check that all three agree.
    {
        rig.reset();
        const Door& door = rig.level.doors[0];

        Light probe;
        probe.position = rig.level.segments[static_cast<size_t>(door.segment)].center;
        probe.segment = door.segment;
        probe.radius = 90.0f;   // easily far enough to reach through
        LightFlood flood;

        rebuildLightSegments(rig.level, probe, flood, &rig.game.doorField);
        const bool blocked = std::find(probe.segments.begin(), probe.segments.end(),
                                       door.neighbour) == probe.segments.end();
        const size_t shutCount = probe.segments.size();

        rig.game.keys[static_cast<size_t>(door.key)] = true;
        rig.teleport(door.center - door.normal * 4.0f, door.segment);
        rig.run(240);
        rebuildLightSegments(rig.level, probe, flood, &rig.game.doorField);
        const bool reaches = std::find(probe.segments.begin(), probe.segments.end(),
                                       door.neighbour) != probe.segments.end();

        ok &= expect(blocked, "a shut door kept the light out of the next segment");
        ok &= expect(reaches, "the same light reached through once the door opened");
        log::info("light through a door: {} segments shut, {} open", shutCount,
                  probe.segments.size());
    }

    // --- the reactor starts the countdown ------------------------------------
    {
        rig.reset();
        int reactor = -1;
        for (int i = 0; i < rig.robots.count; ++i) {
            const sim::Robot& r = rig.robots.robots[static_cast<size_t>(i)];
            if (rig.robotCfg.types[static_cast<size_t>(r.type)].isReactor) reactor = i;
        }
        ok &= expect(reactor >= 0, "the level has a reactor");
        ok &= expect(rig.game.phase == sim::GamePhase::Playing, "the level starts in play");

        if (reactor >= 0) {
            const float health = rig.robots.robots[static_cast<size_t>(reactor)].health;
            rig.robots.robots[static_cast<size_t>(reactor)].health = -1.0f;
            // Robots are stepped here, because it is stepRobots that notices a
            // robot is dead and kills it.
            rig.run(4, true);

            ok &= expect(rig.game.phase == sim::GamePhase::Countdown,
                         "destroying the reactor started the countdown");
            ok &= expect(rig.game.exitOpen, "the exit unlocked");
            ok &= expect(std::abs(rig.game.countdown - rig.gameCfg.countdownSeconds) < 0.5f,
                         "the countdown started at its configured length");
            log::info("reactor: {:.0f} health, destroyed -> countdown {:.1f}s, exit open",
                      health, rig.game.countdown);

            // The shake ramps up as the clock runs down.
            const float shakeAtStart = rig.game.shake;
            rig.run(static_cast<int>(30.0f / kStep));
            const float shakeLater = rig.game.shake;
            ok &= expect(shakeLater > shakeAtStart, "the mine shakes harder as the clock runs out");
            log::info("shake: {:.3f} at the start of the countdown, {:.3f} thirty seconds in",
                      shakeAtStart, shakeLater);
        }
    }

    // --- reaching the exit wins -----------------------------------------------
    {
        rig.reset();
        for (int i = 0; i < rig.robots.count; ++i) {
            const sim::Robot& r = rig.robots.robots[static_cast<size_t>(i)];
            if (rig.robotCfg.types[static_cast<size_t>(r.type)].isReactor) {
                rig.robots.robots[static_cast<size_t>(i)].health = -1.0f;
            }
        }
        rig.run(4, true);
        ok &= expect(rig.game.phase == sim::GamePhase::Countdown, "countdown running");

        rig.teleport(rig.level.exitTrigger.position, rig.level.exitTrigger.segment);
        rig.run(4);
        ok &= expect(rig.game.phase == sim::GamePhase::Won, "reaching the exit won the level");
        log::info("escape: won with {:.1f}s left, {} robots destroyed", rig.game.countdown,
                  rig.game.robotsDestroyed);
    }

    // --- letting the clock run out loses ---------------------------------------
    {
        rig.reset();
        for (int i = 0; i < rig.robots.count; ++i) {
            const sim::Robot& r = rig.robots.robots[static_cast<size_t>(i)];
            if (rig.robotCfg.types[static_cast<size_t>(r.type)].isReactor) {
                rig.robots.robots[static_cast<size_t>(i)].health = -1.0f;
            }
        }
        rig.run(4, true);
        rig.run(static_cast<int>((rig.gameCfg.countdownSeconds + 1.0f) / kStep));
        ok &= expect(rig.game.phase == sim::GamePhase::Lost, "the countdown running out lost");
        log::info("timeout: lost, reason '{}'", rig.game.endReason);
    }

    // --- shields to zero loses --------------------------------------------------
    {
        rig.reset();
        rig.combat.player.shields = 0.0f;
        rig.run(2);
        ok &= expect(rig.game.phase == sim::GamePhase::Lost, "zero shields lost");
        log::info("death: lost, reason '{}'", rig.game.endReason);
    }

    // --- every kind of pickup does something -------------------------------------
    {
        int kinds = 0;
        for (size_t pi = 0; pi < rig.level.pickups.size(); ++pi) {
            rig.reset();
            const Pickup& pickup = rig.level.pickups[pi];

            // Spend something first, so a pickup that tops up has room to.
            rig.combat.player.shields = 40.0f;
            rig.combat.player.energy = 40.0f;
            const sim::PlayerState before = rig.combat.player;
            const auto keysBefore = rig.game.keys;

            rig.teleport(pickup.position, pickup.segment);
            rig.run(4);

            const sim::PlayerState& after = rig.combat.player;
            const bool changed =
                after.shields != before.shields || after.energy != before.energy ||
                after.laserLevel != before.laserLevel ||
                after.concussionAmmo != before.concussionAmmo ||
                after.homingAmmo != before.homingAmmo || after.flares != before.flares ||
                rig.game.keys != keysBefore;

            ok &= expect(rig.game.collected[pi], "the pickup was collected on touch");
            ok &= expect(changed, "the pickup changed something");
            if (changed) ++kinds;
        }
        log::info("pickups: {} of {} collected and had an effect", kinds,
                  rig.level.pickups.size());
    }

    log::info("loop test: {}", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace dvk
