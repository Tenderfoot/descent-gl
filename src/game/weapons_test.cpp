#include "game/weapons_test.h"

#include "core/log.h"
#include "game/input_actions.h"
#include "game/level.h"
#include "sim/collision.h"
#include "sim/combat.h"
#include "sim/props.h"
#include "sim/proxy.h"
#include "sim/robots.h"
#include "sim/weapons.h"

#include <GLFW/glfw3.h>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace dvk {

namespace {

constexpr float kStep = 1.0f / 120.0f;

struct Rig {
    Level level;
    sim::WeaponConfig cfg;
    sim::RobotConfig robotCfg;
    sim::CombatState state;
    sim::RobotState robots;
    sim::PropState props;
    sim::Body ship;
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};

    void reset() {
        state = sim::CombatState{};
        sim::initObjects(level, robotCfg, state.objects);
        sim::spawnRobots(level, robotCfg, robots);
        sim::spawnProps(level, props);
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
            sim::stepCombat(level, state, ship, orientation, cfg, controls, kStep, &robots,
                            nullptr, nullptr, &props);
            sim::stepRobots(level, robotCfg, cfg, robots, state, ship, kStep);
            sim::stepProps(level, props, kStep);
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
        controls.primary = sim::weaponSlot(w) == sim::WeaponSlot::Primary;
        controls.secondary = sim::weaponSlot(w) == sim::WeaponSlot::Secondary;

        const float energyBefore = rig.state.player.energy;
        const int vulcanBefore = rig.state.player.vulcanAmmo;
        const int concussionBefore = rig.state.player.concussionAmmo;
        const int homingBefore = rig.state.player.homingAmmo;

        // The headlight would drain energy underneath the measurement.
        rig.state.player.headlightOn = false;
        rig.run(1, controls);

        // A projectile weapon is counted by what it put in the pool; a hitscan
        // weapon leaves a beam instead, because the damage has already landed
        // by the time anything is drawn.
        const sim::WeaponFire mode = sim::weaponFire(w);
        const auto shotsFired = [&]() {
            return mode == sim::WeaponFire::Projectile ? playerBolts(rig)
                                                       : rig.state.beams.count();
        };
        const int fired = shotsFired();
        ok &= expect(fired == 1, "one shot on the first step");

        // A second step inside the cooldown must not produce another. A
        // continuous weapon has no cooldown - firing every step is the point of
        // it - so what is checked there is that it keeps going.
        rig.run(1, controls);
        if (mode == sim::WeaponFire::Continuous) {
            ok &= expect(rig.state.beams.count() >= 1, "a held beam keeps firing");
        } else if (def.fireInterval > kStep * 1.5f) {
            ok &= expect(shotsFired() == 1,
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

        log::info("{:11} fires: {} {}, {} {:.0f}, damage {:.1f}{}",
                  sim::kWeaponNames[static_cast<size_t>(w)], fired,
                  mode == sim::WeaponFire::Projectile ? "bolt(s)" : "beam(s)",
                  mode == sim::WeaponFire::Projectile ? "speed" : "range",
                  mode == sim::WeaponFire::Projectile ? def.speed : def.range, def.damage,
                  mode == sim::WeaponFire::Continuous ? "/s" : "");
    }

    // --- primary and secondary selections are independent -----------------
    {
        rig.reset();
        sim::FireControls select;
        select.selectWeapon = sim::WeaponVulcan;
        rig.run(1, select);
        select.selectWeapon = sim::WeaponHoming;
        rig.run(1, select);

        ok &= expect(rig.state.player.primaryWeapon == sim::WeaponVulcan,
                     "selecting a secondary preserves the primary selection");
        ok &= expect(rig.state.player.secondaryWeapon == sim::WeaponHoming,
                     "selecting a primary preserves the secondary selection");

        sim::FireControls both;
        both.primary = true;
        both.secondary = true;
        rig.run(1, both);
        bool firedVulcan = false;
        bool firedHoming = false;
        for (const sim::Projectile& p : rig.state.projectiles.items) {
            if (!p.alive || p.owner >= 0) continue;
            firedVulcan = firedVulcan || p.kind == sim::ProjectileKind::Vulcan;
            firedHoming = firedHoming || p.kind == sim::ProjectileKind::Homing;
        }
        ok &= expect(firedVulcan, "primary fire uses the selected primary weapon");
        ok &= expect(firedHoming, "secondary fire uses the selected secondary weapon");
    }

    // --- every gun is reachable, and the missiles cycle -------------------
    //
    // The four primaries have a key each and the missiles share one, so the
    // check that nothing is stranded is that the two lists together name every
    // weapon exactly once - not that either list has a particular length.
    {
        std::array<int, sim::WeaponCount> reachable{};
        for (const WeaponSelectAction& select : kWeaponSelectActions) {
            ++reachable[static_cast<size_t>(select.weapon)];
            ok &= expect(sim::weaponSlot(select.weapon) == sim::WeaponSlot::Primary,
                         "a number key selects a primary weapon");
        }
        for (const sim::WeaponId weapon : sim::kSecondaryWeapons) {
            ++reachable[static_cast<size_t>(weapon)];
            ok &= expect(sim::weaponSlot(weapon) == sim::WeaponSlot::Secondary,
                         "the cycle key selects a secondary weapon");
        }
        bool once = true;
        for (const int count : reachable) once = once && count == 1;
        ok &= expect(once, "every weapon is reachable from exactly one control");

        const InputBindings defaults = defaultInputBindings();
        ok &= expect(defaults[PlayerAction::SelectLaser].code == GLFW_KEY_1 &&
                         defaults[PlayerAction::SelectVulcan].code == GLFW_KEY_2 &&
                         defaults[PlayerAction::SelectLightning].code == GLFW_KEY_3 &&
                         defaults[PlayerAction::SelectRailgun].code == GLFW_KEY_4 &&
                         defaults[PlayerAction::CycleSecondary].code == GLFW_KEY_G,
                     "laser, vulcan, lightning and railgun sit on 1 to 4, missiles on G");

        rig.reset();
        sim::FireControls cycle;
        cycle.cycleSecondary = true;
        // Walk the whole cycle and one step past it, which is where a wrap
        // that runs off the end of the list would show up.
        const int started = rig.state.player.primaryWeapon;
        for (size_t i = 0; i < sim::kSecondaryWeapons.size() + 1; ++i) {
            rig.run(1, cycle);
            const sim::WeaponId expected =
                sim::kSecondaryWeapons[(i + 1) % sim::kSecondaryWeapons.size()];
            ok &= expect(rig.state.player.secondaryWeapon == expected,
                         "the cycle key steps to the next missile and wraps");
        }
        ok &= expect(rig.state.player.primaryWeapon == started,
                     "cycling the missiles leaves the primary weapon alone");

        // A primary selection is not a point in the cycle. Pressing the cycle
        // key while the saved secondary is nonsense has to land on a missile
        // rather than leave a gun on the right trigger.
        rig.reset();
        rig.state.player.secondaryWeapon = sim::WeaponLaser;
        rig.run(1, cycle);
        ok &= expect(sim::weaponSlot(rig.state.player.secondaryWeapon) ==
                         sim::WeaponSlot::Secondary,
                     "cycling from a weapon outside the cycle lands on a missile");

        log::info("selection: keys 1-4 primaries, {} missiles on one cycle key",
                  sim::kSecondaryWeapons.size());
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

    // --- direct hits select a temporary HUD target ---------------------------
    {
        rig.reset();
        rig.state.player.headlightOn = false;
        if (rig.robots.count > 0) {
            const int slot = 0;
            for (int i = 1; i < rig.robots.count; ++i) {
                rig.robots.robots[static_cast<size_t>(i)].alive = false;
            }
            sim::Robot& robot = rig.robots.robots[static_cast<size_t>(slot)];
            const glm::vec3 centre =
                rig.level.segments[static_cast<size_t>(robot.body.segment)].center;
            robot.body.position = centre + glm::vec3(0.0f, 0.0f, -6.0f);
            robot.health = 1000.0f;
            rig.ship.position = centre + glm::vec3(0.0f, 0.0f, 7.0f);
            rig.ship.segment = robot.body.segment;
            rig.aimAt(robot.body.position);

            sim::FireControls controls;
            controls.selectWeapon = sim::WeaponRailgun;
            controls.primary = true;
            rig.run(1, controls);

            ok &= expect(robot.health < 1000.0f, "the railgun directly hit its robot");
            ok &= expect(rig.state.player.lastTargetRobot == slot &&
                             rig.state.player.targetHudTime > 0.0f,
                         "a direct hitscan hit selected its robot for the HUD");

            controls = sim::FireControls{};
            rig.run(static_cast<int>(sim::kTargetHudTime / kStep) + 2, controls);
            ok &= expect(rig.state.player.lastTargetRobot < 0 &&
                             rig.state.player.targetHudTime <= 0.0f,
                         "the HUD target cleared after its timeout");
        }
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
            ok &= expect(rig.state.player.lastTargetRobot == slot,
                         "a direct projectile impact selected its robot for the HUD");
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

    // --- a tall object is solid over its whole height -----------------------
    //
    // Every level object used to be hit-tested as one 2.4 unit sphere on its
    // origin. `pillar.obj` is a column 4.4 x 18 x 4.4, so only the middle 4.8
    // units of it stopped a shot: aim at the top of a pillar and the bolt went
    // through it and hit the wall behind, in full view of the player. The proxy
    // volume is the fix, and this is the case that was failing silently.
    {
        rig.reset();
        rig.state.player.headlightOn = false;

        // The tall one, whichever object that is: twice as high as it is wide.
        // Named by shape rather than by type so a second column of a different
        // name is covered too, and so a level without one reports SKIPPED
        // instead of quietly passing.
        int tall = -1;
        for (size_t i = 0; i < rig.level.objectProxies.size(); ++i) {
            if (!rig.state.objects.alive[i]) continue;
            const sim::PlacedProxy& proxy = rig.level.objectProxies[i];
            if (proxy.kind != sim::ProxyVolume::Kind::Box) continue;
            if (proxy.halfExtents.y > 2.0f * std::max(proxy.halfExtents.x, proxy.halfExtents.z)) {
                tall = static_cast<int>(i);
                break;
            }
        }

        if (tall < 0) {
            log::info("tall object: SKIPPED, no column-shaped object in this level");
        } else {
            const sim::PlacedProxy& proxy = rig.level.objectProxies[static_cast<size_t>(tall)];
            const glm::vec3 centre = proxy.centre;
            const int segment = rig.level.objects[static_cast<size_t>(tall)].segment;

            // Robots stand in rooms and would take the shot first. This test is
            // about the column.
            for (int i = 0; i < rig.robots.count; ++i) rig.robots.robots[static_cast<size_t>(i)].alive = false;

            // The horizontal direction with the most room to back off into.
            const glm::vec3 axes[4] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
            glm::vec3 away(1.0f, 0.0f, 0.0f);
            float room = 0.0f;
            for (const glm::vec3& axis : axes) {
                const sim::RayHit hit = sim::raycast(rig.level, centre, axis, 200.0f, segment);
                const float run = hit.hit ? hit.distance : 200.0f;
                if (run > room) {
                    room = run;
                    away = axis;
                }
            }
            const float back = std::min(9.0f, room - 1.5f);
            const glm::vec3 from = centre + away * back;
            const int fromSegment = findSegment(rig.level, from, segment);
            ok &= expect(fromSegment >= 0, "the firing position beside the column is in the mine");

            // Bottom, middle and top. 0.8 of the half height keeps the aim
            // point inside the column rather than on its cap, where a shot
            // grazing the corner is a fair miss either way.
            const float reach = proxy.halfExtents.y * 0.8f;
            const float heights[3] = {-reach, 0.0f, reach};
            const char* names[3] = {"bottom", "middle", "top"};
            constexpr float kOldRadius = 2.4f;   // what every object used to be

            for (int h = 0; h < 3; ++h) {
                const glm::vec3 target = centre + glm::vec3(0.0f, heights[h], 0.0f);
                const glm::vec3 direction = glm::normalize(target - from);

                sim::CombatTraceParams params;
                params.radius = 0.3f;   // a laser bolt's own thickness
                const sim::CombatHit hit =
                    sim::traceCombatRay(rig.level, rig.state, rig.ship, &rig.robots, from,
                                        direction, 200.0f, fromSegment, params, nullptr,
                                        &rig.props);

                const bool struck =
                    hit.kind == sim::CombatHit::Kind::Object && hit.index == tall;
                ok &= expect(struck, "a shot at a column hits the column");

                // How far the old sphere test was from this shot, so the two
                // ends are provably cases it got wrong rather than cases that
                // happened to work. The perpendicular distance from the
                // object's centre to the ray.
                const glm::vec3 toCentre = centre - from;
                const float alongRay = glm::dot(toCentre, direction);
                const float miss = glm::length(toCentre - direction * alongRay);

                log::info("column {:6}: {} at {:.1f}u; old 2.4u sphere was {:.1f}u off the ray ({})",
                          names[h], struck ? "hit" : "MISSED", hit.distance, miss,
                          miss > kOldRadius + params.radius ? "used to miss" : "used to hit");

                if (h != 1) {
                    ok &= expect(miss > kOldRadius + params.radius,
                                 "the end of the column really is outside the old hit sphere");
                }
            }

            log::info("column: {:.1f} x {:.1f} x {:.1f} at ({:.0f} {:.0f} {:.0f}), solid over all "
                      "{:.0f} units of its height",
                      proxy.halfExtents.x * 2.0f, proxy.halfExtents.y * 2.0f,
                      proxy.halfExtents.z * 2.0f, centre.x, centre.y, centre.z,
                      proxy.halfExtents.y * 2.0f);
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
            ok &= expect(rig.state.player.lastTargetRobot < 0,
                         "splash damage did not select a HUD target");
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

    // --- every weapon throws a muzzle flash ----------------------------------
    //
    // The flash is what makes firing-to-see a tactic in a mine with no ambient
    // light, so it has to carry a light and not just a billboard.
    for (int w = 0; w < sim::WeaponCount; ++w) {
        rig.reset();
        rig.state.player.headlightOn = false;

        sim::FireControls controls;
        controls.selectWeapon = w;
        controls.primary = sim::weaponSlot(w) == sim::WeaponSlot::Primary;
        controls.secondary = sim::weaponSlot(w) == sim::WeaponSlot::Secondary;
        rig.run(1, controls);

        int lit = 0;
        float brightest = 0.0f;
        for (const sim::Effect& e : rig.state.effects.items) {
            if (!e.alive || e.lightIntensity <= 0.0f) continue;
            ++lit;
            brightest = std::max(brightest, e.lightIntensity);
        }
        ok &= expect(lit >= 1, "firing spawned a flash carrying a light");

        const sim::WeaponDef& def = rig.cfg.weapons[static_cast<size_t>(w)];
        ok &= expect(brightest > def.lightIntensity,
                     "the flash is brighter than the bolt it launched");

        // And it is gone again quickly: a muzzle flash that lingers reads as a
        // lamp on the nose of the ship. Counted within a few units of the ship,
        // because by now the bolt has hit something and that impact flash is a
        // lit effect too - just not this one.
        rig.run(static_cast<int>(0.2f / kStep), sim::FireControls{});

        int stillLit = 0;
        for (const sim::Effect& e : rig.state.effects.items) {
            if (!e.alive || e.lightIntensity <= 0.0f) continue;
            if (glm::length(e.position - rig.ship.position) < 4.0f) ++stillLit;
        }
        ok &= expect(stillLit == 0, "the muzzle flash is out again within 0.2 s");

        log::info("{:11} muzzle flash: {} light(s), brightest {:.1f} vs bolt {:.1f}",
                  sim::kWeaponNames[static_cast<size_t>(w)], lit, brightest, def.lightIntensity);
    }

    // --- being hit flinches the screen, and only the screen -------------------
    {
        rig.reset();
        ok &= expect(rig.state.player.hurtTimer == 0.0f, "no flinch before anything lands");

        // The rig steps robots, and this case parks the ship in the open for
        // most of a second. Sixteen of them notice, open fire, and keep
        // re-arming the flinch right through the window this is trying to watch
        // it fade in - so the fade is measured with the mine to itself. The
        // missile below still delivers its damage through the ordinary path,
        // which is what the case is actually about.
        for (sim::Robot& robot : rig.robots.robots) robot.alive = false;
        rig.robots.count = 0;

        // Take a hit the way the game delivers one: a concussion missile into
        // the wall the ship is facing, close enough that its own splash comes
        // back. No special-cased damage call, so this also proves the flinch is
        // wired into the real damage path.
        rig.state.player.headlightOn = false;
        const glm::vec3 centre = rig.level.segments[static_cast<size_t>(rig.ship.segment)].center;
        rig.ship.position = centre;

        // At the nearest side of this room that is actually solid. Aiming
        // straight down looks like the obvious way to drop a missile at your
        // own feet, but the start segment's floor is a portal: the missile
        // leaves the room and detonates far enough away that none of its splash
        // comes back, and the case then only ever registered damage because a
        // robot somewhere else was shooting.
        const Segment& room = rig.level.segments[static_cast<size_t>(rig.ship.segment)];
        glm::vec3 aim = centre;
        float nearest = std::numeric_limits<float>::max();
        for (int side = 0; side < SideCount; ++side) {
            if (room.sides[static_cast<size_t>(side)].isPortal()) continue;
            const std::array<glm::vec3, 4> corner = rig.level.sideCorners(rig.ship.segment, side);
            const glm::vec3 face = (corner[0] + corner[1] + corner[2] + corner[3]) * 0.25f;
            const float d = glm::length(face - centre);
            if (d < nearest) {
                nearest = d;
                aim = face;
            }
        }
        // glm::quatLookAt is degenerate when the look direction is the up
        // vector, so a floor or ceiling gets nudged off the axis.
        if (std::abs(glm::normalize(aim - centre).y) > 0.95f) aim.x += 0.35f;
        rig.aimAt(aim);

        const float before = rig.state.player.shields;
        rig.held = sim::FireControls{};
        rig.held.selectWeapon = sim::WeaponConcussion;
        rig.held.secondary = true;
        rig.run(1);
        rig.held = sim::FireControls{};

        const glm::vec3 shipAt = rig.ship.position;
        const glm::vec3 shipVel = rig.ship.velocity;

        // Run until it lands rather than guessing how long a slow missile takes
        // to cross a room, so the flinch is checked while it is still on.
        // Where it went off is the last place the missile was seen alive.
        glm::vec3 blastAt = rig.ship.position;
        int waited = 0;
        while (waited < 600 && rig.state.player.shields >= before) {
            for (const sim::Projectile& live : rig.state.projectiles.items) {
                if (live.alive && live.owner < 0) blastAt = live.position;
            }
            rig.run(1);
            ++waited;
        }

        ok &= expect(rig.state.player.shields < before, "the ship's own splash took shields");
        ok &= expect(rig.state.player.hurtTimer > 0.0f, "the hit set the flinch");
        // The flinch is screen-only, and the flight channel stays the flight
        // model's: a blast does not fly the ship. It does shove it, and that
        // shove arrives in the knockback channel where nothing in stepShip can
        // clamp it away.
        ok &= expect(rig.ship.position == shipAt && rig.ship.velocity == shipVel,
                     "the flinch left the flight channel alone");
        ok &= expect(glm::length(rig.ship.knockback) > 0.0f,
                     "the blast shoved the ship");
        ok &= expect(glm::dot(glm::normalize(rig.ship.knockback),
                              glm::normalize(rig.ship.position - blastAt)) > 0.0f,
                     "the shove points away from the blast");

        rig.run(static_cast<int>((sim::kHurtFlashTime + 0.05f) / kStep));
        ok &= expect(rig.state.player.hurtTimer == 0.0f, "the flinch faded out");
        log::info("damage flinch: {:.0f} shields lost, faded in {:.2f}s", before -
                  rig.state.player.shields, sim::kHurtFlashTime);
    }

    // --- a disabled ship has no guns and no headlight -------------------------
    {
        rig.reset();
        for (sim::Robot& robot : rig.robots.robots) robot.alive = false;
        rig.robots.count = 0;
        rig.state.player.headlightOn = true;

        // Under the threshold, still alive.
        rig.state.player.shields = sim::kPlayerMaxShields * 0.09f;
        ok &= expect(sim::playerDisabled(rig.state.player), "9% shields disables the ship");

        const int boltsBefore = rig.state.projectiles.count();
        sim::FireControls everything;
        everything.selectWeapon = sim::WeaponLaser;
        everything.primary = true;
        everything.secondary = true;
        everything.flare = true;
        const int flaresBefore = rig.state.player.flares;
        rig.run(120, everything);

        ok &= expect(rig.state.projectiles.count() == boltsBefore,
                     "a disabled ship fires nothing");
        ok &= expect(rig.state.beams.count() == 0, "a disabled ship raises no beam");
        ok &= expect(rig.state.player.flares == flaresBefore, "a disabled ship throws no flare");
        ok &= expect(!rig.state.player.headlightOn, "a disabled ship's headlight goes out");

        // A shield pickup hands it straight back: the state is derived from
        // shields every step, never latched.
        rig.state.player.shields = sim::kPlayerMaxShields * 0.5f;
        ok &= expect(!sim::playerDisabled(rig.state.player), "repairing past 10% restores control");
        rig.run(1, everything);
        ok &= expect(rig.state.projectiles.count() > boltsBefore, "and the guns work again");

        log::info("disabled player: one second of every trigger at 9% shields fired nothing");
    }

    // --- a hitscan shot never leaves the mine --------------------------------
    //
    // The projectile equivalent of this is the no-tunnelling case: a bolt fast
    // enough to cross a wall in one step must still be stopped by it. A trace
    // has no step to cross, so the question becomes whether it stops at all.
    // The mine is closed, so a 300-unit ray fired from anywhere in any
    // direction must end on something. A ray that runs to full range has left
    // the world.
    {
        rig.reset();
        const sim::WeaponDef& rail = rig.cfg.weapons[sim::WeaponRailgun];

        std::array<glm::vec3, 26> dirs{};
        size_t made = 0;
        for (int x = -1; x <= 1; ++x) {
            for (int y = -1; y <= 1; ++y) {
                for (int z = -1; z <= 1; ++z) {
                    if (x == 0 && y == 0 && z == 0) continue;
                    dirs[made++] = glm::normalize(glm::vec3(x, y, z));
                }
            }
        }

        int escaped = 0;
        float longest = 0.0f;
        for (int si = 0; si < rig.level.segmentCount(); ++si) {
            const glm::vec3 from = rig.level.segments[static_cast<size_t>(si)].center;
            for (const glm::vec3& dir : dirs) {
                const sim::CombatHit hit = sim::traceCombatRay(
                    rig.level, rig.state, rig.ship, &rig.robots, from, dir, rail.range, si);
                if (hit.kind == sim::CombatHit::Kind::None) {
                    ++escaped;
                    continue;
                }
                longest = std::max(longest, hit.distance);
            }
        }

        ok &= expect(escaped == 0, "no hitscan shot ran past the end of the mine");
        log::info("hitscan containment: {} traces at {:.0f}u range, {} escaped, longest {:.1f}u",
                  rig.level.segmentCount() * static_cast<int>(dirs.size()), rail.range, escaped,
                  longest);
    }

    // --- a body hit belongs to the room containing the body -----------------
    {
        int fromSegment = -1;
        int targetSegment = -1;
        for (int si = 0; si < rig.level.segmentCount() && targetSegment < 0; ++si) {
            const Segment& room = rig.level.segments[static_cast<size_t>(si)];
            for (const SideData& side : room.sides) {
                if (side.isPortal() && side.door < 0) {
                    fromSegment = si;
                    targetSegment = side.neighbour;
                    break;
                }
            }
        }
        ok &= expect(targetSegment >= 0, "the level has an open portal for a cross-room trace");

        if (targetSegment >= 0) {
            sim::CombatState empty;
            sim::Body target;
            const glm::vec3 origin =
                rig.level.segments[static_cast<size_t>(fromSegment)].center;
            target.position = rig.level.segments[static_cast<size_t>(targetSegment)].center;
            target.segment = targetSegment;
            const glm::vec3 toTarget = target.position - origin;

            sim::CombatTraceParams hostile;
            hostile.owner = 0;   // robot fire includes the player body
            const sim::CombatHit hit = sim::traceCombatRay(
                rig.level, empty, target, nullptr, origin, glm::normalize(toTarget),
                glm::length(toTarget) + 20.0f, fromSegment, hostile);

            ok &= expect(hit.kind == sim::CombatHit::Kind::Player,
                         "a trace through an open portal hits the body in the next room");
            ok &= expect(hit.segment == targetSegment,
                         "a cross-room body hit reports the target room");
            log::info("cross-room hit: segment {} -> {}, impact belongs to {}", fromSegment,
                      targetSegment, hit.segment);
        }
    }

    // --- a held beam is metered per second, not per step ----------------------
    {
        rig.reset();
        for (sim::Robot& robot : rig.robots.robots) robot.alive = false;
        rig.robots.count = 0;
        rig.state.player.headlightOn = false;

        const sim::WeaponDef& arc = rig.cfg.weapons[sim::WeaponLightning];
        sim::FireControls held;
        held.selectWeapon = sim::WeaponLightning;
        held.primary = true;

        // One second of held trigger, a step at a time. What comes off the
        // energy bar must be the per-second number in the config, not that
        // number once per step - which at 120 Hz would empty the bar instantly.
        const float before = rig.state.player.energy;
        rig.run(120, held);
        const float spent = before - rig.state.player.energy;

        ok &= expect(std::abs(spent - arc.energyCost) < arc.energyCost * 0.05f,
                     "one second of beam costs one second of energy");
        ok &= expect(rig.state.beams.count() > 0, "the beam is still up while held");

        // And it stops the moment the trigger does.
        rig.run(static_cast<int>(arc.beamTime / kStep) + 2, sim::FireControls{});
        ok &= expect(rig.state.beams.count() == 0, "the beam goes out when the trigger does");

        log::info("held beam: {:.1f} energy over 1.0 s against {:.1f}/s configured", spent,
                  arc.energyCost);
    }

    // --- shooting a floating light moves it ----------------------------------
    //
    // Both delivery paths, because they reach the prop through different code:
    // the railgun through the hitscan branch, the concussion through the
    // projectile loop. Both are nose-mounted. The laser is not - it alternates
    // wingtips 1.75 units off the centreline, which is wider than a 1.1-radius
    // ball, so aiming a laser dead at one of these and missing is correct.
    {
        for (const int weapon : {sim::WeaponRailgun, sim::WeaponConcussion}) {
            rig.reset();
            for (sim::Robot& robot : rig.robots.robots) robot.alive = false;
            rig.robots.count = 0;

            if (rig.props.floatingCount == 0) {
                log::info("floating light: SKIPPED, this level has none");
                break;
            }

            sim::FloatingLight& ball = rig.props.floating[0];
            rig.ship.position = ball.body.position + glm::vec3(0.0f, 0.0f, 12.0f);
            rig.ship.segment = ball.body.segment;
            rig.aimAt(ball.body.position);

            const glm::vec3 restingAt = ball.body.position;
            const glm::vec3 fromShip = glm::normalize(ball.body.position - rig.ship.position);

            sim::FireControls shoot;
            shoot.selectWeapon = weapon;
            shoot.primary = sim::weaponSlot(weapon) == sim::WeaponSlot::Primary;
            shoot.secondary = sim::weaponSlot(weapon) == sim::WeaponSlot::Secondary;
            rig.run(1, shoot);
            // Long enough for a missile to cross the gap and land.
            rig.run(60, sim::FireControls{});

            const glm::vec3 moved = ball.body.position - restingAt;
            const char* name = sim::kWeaponNames[static_cast<size_t>(weapon)];
            ok &= expect(glm::length(moved) > 0.5f, "shooting a floating light moves it");
            ok &= expect(glm::dot(glm::normalize(moved), fromShip) > 0.0f,
                         "it moves away from the shot, not towards it");
            // It is a light, not a target: shooting it never takes it away.
            ok &= expect(ball.alive, "shooting a floating light does not destroy it");

            log::info("floating light: {} moved it {:.2f}u away in 0.5 s", name,
                      glm::length(moved));
        }
    }

    // --- a blast does not reach through rock ---------------------------------
    {
        rig.reset();
        for (sim::Robot& robot : rig.robots.robots) robot.alive = false;
        rig.robots.count = 0;

        const glm::vec3 centre = rig.level.segments[static_cast<size_t>(rig.ship.segment)].center;
        rig.ship.position = centre;

        // Aim at a side of this room that is solid, and find where it is.
        const Segment& room = rig.level.segments[static_cast<size_t>(rig.ship.segment)];
        glm::vec3 toWall(0.0f);
        float wallAt = 0.0f;
        for (int side = 0; side < SideCount; ++side) {
            if (room.sides[static_cast<size_t>(side)].isPortal()) continue;
            const std::array<glm::vec3, 4> corner = rig.level.sideCorners(rig.ship.segment, side);
            const glm::vec3 face = (corner[0] + corner[1] + corner[2] + corner[3]) * 0.25f;
            const glm::vec3 to = face - centre;
            const float d = glm::length(to);
            if (d < 1e-3f) continue;
            const sim::RayHit hit =
                sim::raycast(rig.level, centre, to / d, 400.0f, rig.ship.segment);
            if (!hit.hit) continue;
            if (wallAt == 0.0f || hit.distance < wallAt) {
                wallAt = hit.distance;
                toWall = to / d;
            }
        }
        ok &= expect(wallAt > 0.0f, "the room has a solid wall to hide behind");

        // Detonates wherever it is put, on the next step, with a radius wide
        // enough to cover the ship twice over. Straight into the pool rather
        // than fired, because the point is to choose where it goes off.
        const auto detonate = [&](const glm::vec3& at) {
            const int slot = rig.state.projectiles.spawn();
            if (slot < 0) return;
            sim::Projectile& bomb = rig.state.projectiles.items[static_cast<size_t>(slot)];
            bomb.kind = sim::ProjectileKind::Concussion;
            bomb.position = at;
            bomb.segment = rig.ship.segment;
            bomb.velocity = glm::vec3(0.0f);
            bomb.lifetime = 0.0f;
            bomb.splashRadius = wallAt * 3.0f;
            bomb.splashDamage = 60.0f;
            bomb.splashImpulse = 90.0f;
            rig.run(1);
        };

        // One unit past the wall: well inside the blast radius, and on the
        // wrong side of solid rock.
        const float shieldsBefore = rig.state.player.shields;
        detonate(centre + toWall * (wallAt + 1.0f));
        ok &= expect(rig.state.player.shields == shieldsBefore,
                     "a blast through a wall does no damage");
        ok &= expect(rig.ship.knockback == glm::vec3(0.0f),
                     "a blast through a wall does not shove");

        // The same blast, the same distance, with nothing in the way.
        rig.ship.knockback = glm::vec3(0.0f);
        detonate(centre + toWall * (wallAt * 0.5f));
        ok &= expect(rig.state.player.shields < shieldsBefore,
                     "the same blast in the open does damage");
        ok &= expect(glm::length(rig.ship.knockback) > 0.0f,
                     "the same blast in the open shoves");

        log::info("blast occlusion: wall at {:.1f}u, through it 0 shields, around it {:.0f}",
                  wallAt, shieldsBefore - rig.state.player.shields);
    }

    // --- a missile kicks the camera, a gun does not ---------------------------
    {
        rig.reset();
        sim::FireControls laser;
        laser.selectWeapon = sim::WeaponLaser;
        laser.primary = true;
        rig.run(1, laser);
        ok &= expect(rig.state.player.recoilTimer == 0.0f, "the laser does not kick");

        rig.reset();
        sim::FireControls missile;
        missile.selectWeapon = sim::WeaponConcussion;
        missile.secondary = true;
        const glm::vec3 velBefore = rig.ship.velocity;
        rig.run(1, missile);
        ok &= expect(rig.state.player.recoilTimer > 0.0f, "the missile kicked");
        ok &= expect(rig.ship.velocity == velBefore, "the kick did not push the ship");

        rig.run(static_cast<int>((sim::kRecoilTime + 0.05f) / kStep), sim::FireControls{});
        ok &= expect(rig.state.player.recoilTimer == 0.0f, "the kick faded out");
        log::info("missile kick: {:.2f}s, ship velocity unchanged", sim::kRecoilTime);
    }

    log::info("weapons test: {}", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace dvk
