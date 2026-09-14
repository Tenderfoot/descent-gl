#include "game/weapons_test.h"

#include "core/log.h"
#include "game/level.h"
#include "sim/collision.h"
#include "sim/combat.h"
#include "sim/robots.h"
#include "sim/weapons.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace dvk {

namespace {

constexpr float kStep = 1.0f / 120.0f;

struct Rig {
    Level level;
    sim::WeaponConfig cfg;
    sim::RobotConfig robotCfg;
    sim::CombatState state;
    sim::RobotState robots;
    sim::Body ship;
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};

    void reset() {
        state = sim::CombatState{};
        sim::initObjects(level, robotCfg, state.objects);
        sim::spawnRobots(level, robotCfg, robots);
        ship = sim::Body{};
        ship.position = level.start.position;
        ship.segment = level.start.segment;
        orientation = level.start.orientation;
    }

    // The controls used by run(int), so the target tests can re-aim between
    // steps without rebuilding them.
    sim::FireControls held;

    void run(int steps) { run(steps, held); }

    void run(int steps, const sim::FireControls& controls) {
        for (int i = 0; i < steps; ++i) {
            // Robots are stepped too, so a target shot at here behaves the
            // way it does in the game rather than sitting still for the test.
            sim::stepCombat(level, state, ship, orientation, cfg, controls, kStep, &robots);
            sim::stepRobots(level, robotCfg, cfg, robots, state, ship, kStep);
        }
    }

    // Point the ship at a world position, so a test can aim without flying.
    void aimAt(const glm::vec3& target) {
        const glm::vec3 to = target - ship.position;
        if (glm::length(to) < 1e-4f) return;
        orientation = glm::quatLookAt(glm::normalize(to), glm::vec3(0.0f, 1.0f, 0.0f));
    }
};

// Only the player's shots. Robots fire into the same pool, so a plain count
// of live projectiles also counts whatever is being shot AT the player - which
// on a level with robots near the spawn is never zero.
int playerBolts(const Rig& rig) {
    int n = 0;
    for (const sim::Projectile& p : rig.state.projectiles.items) {
        if (p.alive && p.owner < 0) ++n;
    }
    return n;
}

bool expect(bool condition, const char* what) {
    if (!condition) log::error("FAILED: {}", what);
    return condition;
}

} // namespace

int runWeaponsTest(const char* levelPath, const char* weaponConfigPath,
                   const char* robotConfigPath) {
    Rig rig;
    rig.level = loadLevel(levelPath);
    rig.cfg = sim::loadWeaponConfig(weaponConfigPath);
    rig.robotCfg = sim::loadRobotConfig(robotConfigPath);
    bool ok = true;

    // --- every weapon fires, costs what it should, and respects its rate ----
    for (int w = 0; w < sim::WeaponCount; ++w) {
        rig.reset();
        const sim::WeaponDef& def = rig.cfg.weapons[static_cast<size_t>(w)];

        sim::FireControls controls;
        controls.selectWeapon = w;
        controls.primary = w <= sim::WeaponVulcan;
        controls.secondary = w >= sim::WeaponConcussion;

        const float energyBefore = rig.state.player.energy;
        const int vulcanBefore = rig.state.player.vulcanAmmo;
        const int concussionBefore = rig.state.player.concussionAmmo;
        const int homingBefore = rig.state.player.homingAmmo;

        // The headlight would drain energy underneath the measurement.
        rig.state.player.headlightOn = false;
        rig.run(1, controls);

        const int fired = playerBolts(rig);
        ok &= expect(fired == 1, "one shot on the first step");

        // A second step inside the cooldown must not produce another.
        rig.run(1, controls);
        const bool withinCooldown = def.fireInterval > kStep * 1.5f;
        if (withinCooldown) {
            ok &= expect(playerBolts(rig) == 1,
                         "no second shot while the weapon is still cooling down");
        }

        if (def.energyCost > 0.0f) {
            ok &= expect(rig.state.player.energy < energyBefore, "firing cost energy");
        }
        if (w == sim::WeaponVulcan) {
            ok &= expect(rig.state.player.vulcanAmmo < vulcanBefore, "vulcan spent ammo");
        }
        if (w == sim::WeaponConcussion) {
            ok &= expect(rig.state.player.concussionAmmo < concussionBefore,
                         "concussion spent ammo");
        }
        if (w == sim::WeaponHoming) {
            ok &= expect(rig.state.player.homingAmmo < homingBefore, "homing spent ammo");
        }

        log::info("{:11} fires: {} bolt(s), speed {:.0f}, damage {:.1f}",
                  sim::kWeaponNames[static_cast<size_t>(w)], fired, def.speed, def.damage);
    }

    // --- bolts hit walls rather than passing through them -------------------
    {
        rig.reset();
        rig.state.player.headlightOn = false;
        sim::FireControls controls;
        controls.selectWeapon = sim::WeaponLaser;
        controls.primary = true;
        rig.run(1, controls);

        controls.primary = false;
        // Step until the bolt is gone, so the impact effect can be counted
        // while it is still alive - they last a sixth of a second.
        int steps = 0;
        while (playerBolts(rig) > 0 && steps < 240) {
            rig.run(1, controls);
            ++steps;
        }
        ok &= expect(playerBolts(rig) == 0, "the bolt hit something and expired");
        ok &= expect(rig.state.effects.count() > 0, "the impact spawned a flash");
        log::info("wall impact: bolt gone after {:.2f}s, {} impact flash(es) alive",
                  static_cast<float>(steps) * kStep, rig.state.effects.count());
    }

    // --- a very fast bolt still cannot tunnel -------------------------------
    //
    // The swept sphere exists for exactly this. At 2000 units/s a bolt covers
    // 16 units in one 120 Hz step, most of a segment, and a position-only test
    // would step straight through the wall.
    {
        rig.reset();
        rig.state.player.headlightOn = false;
        const float saved = rig.cfg.weapons[sim::WeaponLaser].speed;
        rig.cfg.weapons[sim::WeaponLaser].speed = 2000.0f;

        sim::FireControls controls;
        controls.selectWeapon = sim::WeaponLaser;
        controls.primary = true;
        rig.run(1, controls);
        controls.primary = false;
        rig.run(60, controls);

        const bool stopped = playerBolts(rig) == 0;
        ok &= expect(stopped, "a 2000 u/s bolt still stopped at a wall");
        log::info("tunnelling: a bolt at 2000 u/s (16.7 units per step) {}",
                  stopped ? "stopped at the wall" : "ESCAPED");
        rig.cfg.weapons[sim::WeaponLaser].speed = saved;
    }

    // --- targets take damage and die ----------------------------------------
    //
    // The targets are robots from M6 on. They are stepped normally, so this is
    // also a check that a robot being shot at reacts rather than standing
    // still: it will retreat once it is hurt enough, which is why the loop
    // re-aims every step.
    {
        rig.reset();
        rig.state.player.headlightOn = false;
        ok &= expect(rig.robots.count > 0, "the level has robots to shoot");

        if (rig.robots.count > 0) {
            const int slot = 0;
            sim::Robot& robot = rig.robots.robots[static_cast<size_t>(slot)];
            const sim::RobotDef& def = rig.robotCfg.types[static_cast<size_t>(robot.type)];

            const glm::vec3 centre =
                rig.level.segments[static_cast<size_t>(robot.body.segment)].center;
            robot.body.position = centre + glm::vec3(0.0f, 0.0f, -6.0f);
            rig.ship.position = centre + glm::vec3(0.0f, 0.0f, 7.0f);
            rig.ship.segment = robot.body.segment;

            const float health = robot.health;
            const float damage = rig.cfg.laserDamage[0];

            rig.held = sim::FireControls{};
            rig.held.selectWeapon = sim::WeaponLaser;
            rig.held.primary = true;

            int steps = 0;
            while (rig.robots.robots[static_cast<size_t>(slot)].alive && steps < 2400) {
                rig.aimAt(rig.robots.robots[static_cast<size_t>(slot)].body.position);
                rig.run(1);
                ++steps;
            }
            rig.held = sim::FireControls{};

            const bool destroyed = !rig.robots.robots[static_cast<size_t>(slot)].alive;
            ok &= expect(destroyed, "the robot was destroyed by sustained laser fire");
            log::info("target: {} with {:.0f} health, {:.0f} damage per shot, destroyed after "
                      "{:.2f}s",
                      def.name, health, damage, static_cast<float>(steps) * kStep);
        }
    }

    // --- a robot's spawn point is not a target once the robot has moved ------
    //
    // Robots come from the level's object list, and the object list is also
    // what bolts test against. Every robot's spawn point used to stay behind
    // as an invisible, indestructible sphere that stopped shots for no visible
    // reason. Here the robot is removed and a bolt is fired straight through
    // where it stood; it has to reach the wall beyond.
    {
        rig.reset();
        rig.state.player.headlightOn = false;
        if (rig.robots.count > 0) {
            sim::Robot& robot = rig.robots.robots[0];
            const glm::vec3 spawn = robot.body.position;
            const int segment = robot.body.segment;
            robot.alive = false;

            // The axis with the most room on both sides of the spawn point:
            // behind it for the ship to sit, ahead of it for the bolt to pass
            // the spawn point and still fly on.
            const glm::vec3 axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                       {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            const auto clearRun = [&](const glm::vec3& d) {
                const sim::RayHit hit = sim::raycast(rig.level, spawn, d, 200.0f, segment);
                return hit.hit ? hit.distance : 200.0f;
            };
            glm::vec3 dir(0.0f, 0.0f, -1.0f);
            float clear = 0.0f;
            float room = 0.0f;
            for (const glm::vec3& axis : axes) {
                const float ahead = clearRun(axis);
                const float behindRun = clearRun(-axis);
                if (std::min(ahead, behindRun) > std::min(clear, room)) {
                    clear = ahead;
                    room = behindRun;
                    dir = axis;
                }
            }
            const float back = std::min(5.0f, room - 1.0f);
            rig.ship.position = spawn - dir * back;
            rig.ship.segment = findSegment(rig.level, rig.ship.position, segment);
            rig.aimAt(spawn);
            ok &= expect(rig.ship.segment >= 0, "the firing position is inside the mine");

            rig.held = sim::FireControls{};
            rig.held.selectWeapon = sim::WeaponLaser;
            rig.held.primary = true;
            rig.run(1);
            rig.held = sim::FireControls{};

            float furthest = 0.0f;
            for (int i = 0; i < 240 && playerBolts(rig) > 0; ++i) {
                rig.run(1);
                for (const sim::Projectile& p : rig.state.projectiles.items) {
                    if (!p.alive || p.owner >= 0) continue;
                    furthest = std::max(furthest, glm::length(p.position - rig.ship.position));
                }
            }
            // Past the spawn point by more than the old phantom's 2.4 unit
            // radius plus the bolt's own.
            const bool passed = furthest > back + 4.0f;
            ok &= expect(passed, "a bolt flew through a dead robot's spawn point");
            log::info("phantom: bolt reached {:.1f} units past a spawn point with {:.1f} units of "
                      "clear run ({})",
                      furthest - back, clear, passed ? "passed through" : "STOPPED");
        }
    }

    // --- a homing missile seeks a robot -------------------------------------
    {
        rig.reset();
        rig.state.player.headlightOn = false;
        if (rig.robots.count > 0) {
            const int slot = 0;
            sim::Robot& robot = rig.robots.robots[static_cast<size_t>(slot)];
            const glm::vec3 centre =
                rig.level.segments[static_cast<size_t>(robot.body.segment)].center;
            robot.body.position = centre + glm::vec3(0.0f, 0.0f, -7.0f);
            rig.ship.position = centre + glm::vec3(0.0f, 0.0f, 7.0f);
            rig.ship.segment = robot.body.segment;
            // Aimed a little off, so only a missile that turns can connect.
            rig.aimAt(robot.body.position + glm::vec3(3.0f, 0.0f, 0.0f));

            const float before = robot.health;
            rig.held = sim::FireControls{};
            rig.held.selectWeapon = sim::WeaponHoming;
            rig.held.secondary = true;
            rig.run(1);
            rig.held = sim::FireControls{};

            int acquired = -1;
            for (const sim::Projectile& p : rig.state.projectiles.items) {
                if (p.alive && p.owner < 0 && p.kind == sim::ProjectileKind::Homing) {
                    acquired = p.targetRobot;
                }
            }
            ok &= expect(acquired == slot, "the missile locked on to the robot");
            rig.run(240);
            const float after = rig.robots.robots[static_cast<size_t>(slot)].alive
                                    ? rig.robots.robots[static_cast<size_t>(slot)].health
                                    : 0.0f;
            ok &= expect(after < before, "the missile hit the robot it locked on to");
            log::info("homing: locked robot {}, target health {:.0f} -> {:.1f}", acquired, before,
                      after);
        }
    }

    // --- splash damage ------------------------------------------------------
    {
        rig.reset();
        rig.state.player.headlightOn = false;
        if (rig.robots.count > 0) {
            const int slot = 0;
            sim::Robot& robot = rig.robots.robots[static_cast<size_t>(slot)];
            const glm::vec3 centre =
                rig.level.segments[static_cast<size_t>(robot.body.segment)].center;
            robot.body.position = centre + glm::vec3(0.0f, 0.0f, -6.0f);
            rig.ship.position = centre + glm::vec3(0.0f, 0.0f, 8.0f);
            rig.ship.segment = robot.body.segment;

            const float before = robot.health;
            // Deliberately aimed past it, so only the splash can reach.
            rig.aimAt(robot.body.position + glm::vec3(5.0f, 0.0f, -6.0f));

            rig.held = sim::FireControls{};
            rig.held.selectWeapon = sim::WeaponConcussion;
            rig.held.secondary = true;
            rig.run(1);
            rig.held.secondary = false;
            rig.run(180);

            const float after = rig.robots.robots[static_cast<size_t>(slot)].alive
                                    ? rig.robots.robots[static_cast<size_t>(slot)].health
                                    : 0.0f;
            ok &= expect(after < before, "a near miss still damaged the target");
            log::info("splash: near miss took the target from {:.0f} to {:.1f} health", before,
                      after);
        }
    }

    // --- flares stick, burn, and go out --------------------------------------
    {
        rig.reset();
        rig.state.player.headlightOn = false;
        sim::FireControls controls;
        controls.flare = true;
        rig.run(1, controls);
        controls.flare = false;

        ok &= expect(rig.state.player.flares == 11, "throwing a flare spent one");

        int landedAt = -1;
        for (int i = 0; i < 600 && landedAt < 0; ++i) {
            rig.run(1, controls);
            if (rig.state.flares.count() > 0) landedAt = i;
        }
        ok &= expect(landedAt >= 0, "the flare landed and became a light");
        log::info("flare: landed after {:.2f}s, burns for {:.0f}s", static_cast<float>(landedAt) * kStep,
                  rig.cfg.flareLifetime);

        // Run out its life and confirm it goes out rather than burning forever.
        const int lifeSteps = static_cast<int>(rig.cfg.flareLifetime / kStep) + 240;
        rig.run(lifeSteps, controls);
        ok &= expect(rig.state.flares.count() == 0, "the flare went out at the end of its life");
    }

    // --- sustained vulcan fire does not overrun the pool ---------------------
    {
        rig.reset();
        rig.state.player.headlightOn = false;
        sim::FireControls controls;
        controls.selectWeapon = sim::WeaponVulcan;
        controls.primary = true;
        rig.run(600, controls);

        const int live = playerBolts(rig);
        ok &= expect(live <= sim::kMaxProjectiles, "the projectile pool held");
        ok &= expect(rig.state.player.vulcanAmmo < 2000, "sustained fire spent ammo");
        log::info("vulcan spam: 5 s of fire, {} bolts live, {} rounds left, pool capacity {}",
                  live, rig.state.player.vulcanAmmo, sim::kMaxProjectiles);
    }

    // --- the headlight costs energy ------------------------------------------
    {
        rig.reset();
        rig.state.player.headlightOn = true;
        const float before = rig.state.player.energy;
        const sim::FireControls idle;
        rig.run(120, idle);   // one second
        const float drained = before - rig.state.player.energy;
        ok &= expect(std::abs(drained - rig.cfg.headlightDrain) < 0.05f,
                     "the headlight drained one second's worth of energy");
        log::info("headlight: {:.2f} energy per second (configured {:.2f})", drained,
                  rig.cfg.headlightDrain);

        // Run it dry and confirm it switches itself off.
        rig.run(static_cast<int>(100.0f / rig.cfg.headlightDrain / kStep) + 240, idle);
        ok &= expect(!rig.state.player.headlightOn, "the headlight went out when energy ran out");
        log::info("headlight: switched off at {:.2f} energy", rig.state.player.energy);
    }

    // --- death resets to the player start -------------------------------------
    {
        rig.reset();
        rig.ship.position = rig.level.segments[7].center;
        rig.ship.segment = 7;
        rig.state.player.shields = 0.0f;

        const bool died = sim::checkPlayerDeath(rig.level, rig.state, rig.ship, rig.orientation);
        ok &= expect(died, "zero shields is a death");
        ok &= expect(rig.ship.segment == rig.level.start.segment, "respawned at the player start");
        ok &= expect(rig.state.player.shields > 99.0f, "shields restored on respawn");
        log::info("death: reset to segment {}, shields {:.0f}, deaths {}", rig.ship.segment,
                  rig.state.player.shields, rig.state.player.deaths);
    }

    log::info("weapons test: {}", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace dvk
