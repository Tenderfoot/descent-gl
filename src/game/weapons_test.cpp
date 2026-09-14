#include "game/weapons_test.h"

#include "core/log.h"
#include "game/level.h"
#include "sim/combat.h"
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
    sim::CombatState state;
    sim::Body ship;
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};

    void reset() {
        state = sim::CombatState{};
        sim::initObjects(level, state.objects);
        ship = sim::Body{};
        ship.position = level.start.position;
        ship.segment = level.start.segment;
        orientation = level.start.orientation;
    }

    void run(int steps, const sim::FireControls& controls) {
        for (int i = 0; i < steps; ++i) {
            sim::stepCombat(level, state, ship, orientation, cfg, controls, kStep);
        }
    }

    // Point the ship at a world position, so a test can aim without flying.
    void aimAt(const glm::vec3& target) {
        const glm::vec3 to = target - ship.position;
        if (glm::length(to) < 1e-4f) return;
        orientation = glm::quatLookAt(glm::normalize(to), glm::vec3(0.0f, 1.0f, 0.0f));
    }
};

bool expect(bool condition, const char* what) {
    if (!condition) log::error("FAILED: {}", what);
    return condition;
}

} // namespace

int runWeaponsTest(const char* levelPath, const char* weaponConfigPath) {
    Rig rig;
    rig.level = loadLevel(levelPath);
    rig.cfg = sim::loadWeaponConfig(weaponConfigPath);
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

        const int fired = rig.state.projectiles.count();
        ok &= expect(fired == 1, "one shot on the first step");

        // A second step inside the cooldown must not produce another.
        rig.run(1, controls);
        const bool withinCooldown = def.fireInterval > kStep * 1.5f;
        if (withinCooldown) {
            ok &= expect(rig.state.projectiles.count() == 1,
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
        while (rig.state.projectiles.count() > 0 && steps < 240) {
            rig.run(1, controls);
            ++steps;
        }
        ok &= expect(rig.state.projectiles.count() == 0, "the bolt hit something and expired");
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

        const bool stopped = rig.state.projectiles.count() == 0;
        ok &= expect(stopped, "a 2000 u/s bolt still stopped at a wall");
        log::info("tunnelling: a bolt at 2000 u/s (16.7 units per step) {}",
                  stopped ? "stopped at the wall" : "ESCAPED");
        rig.cfg.weapons[sim::WeaponLaser].speed = saved;
    }

    // --- targets take damage and die ----------------------------------------
    {
        rig.reset();
        rig.state.player.headlightOn = false;

        int target = -1;
        for (int i = 0; i < rig.state.objects.count; ++i) {
            if (rig.level.objects[static_cast<size_t>(i)].health > 0.0f) {
                target = i;
                break;
            }
        }
        ok &= expect(target >= 0, "the level has at least one destructible target");

        if (target >= 0) {
            const LevelObject& obj = rig.level.objects[static_cast<size_t>(target)];
            // Stand off from the target and shoot it, rather than flying there.
            rig.ship.position = obj.position + glm::vec3(0.0f, 0.0f, 14.0f);
            rig.ship.segment = findSegment(rig.level, rig.ship.position, obj.segment);
            rig.aimAt(obj.position);

            const float health = obj.health;
            const float damage = rig.cfg.laserDamage[0];
            const int expectedShots = static_cast<int>(std::ceil(health / damage));

            sim::FireControls controls;
            controls.selectWeapon = sim::WeaponLaser;
            controls.primary = true;

            int steps = 0;
            while (rig.state.objects.alive[static_cast<size_t>(target)] && steps < 1200) {
                rig.run(1, controls);
                ++steps;
            }

            const bool destroyed = !rig.state.objects.alive[static_cast<size_t>(target)];
            ok &= expect(destroyed, "the target was destroyed by sustained laser fire");
            log::info("target {}: {:.0f} health, {:.0f} damage per shot, {} shots needed, "
                      "destroyed after {:.2f}s",
                      target, health, damage, expectedShots, static_cast<float>(steps) * kStep);
        }
    }

    // --- splash damage ------------------------------------------------------
    {
        rig.reset();
        rig.state.player.headlightOn = false;
        int target = -1;
        for (int i = 0; i < rig.state.objects.count; ++i) {
            if (rig.level.objects[static_cast<size_t>(i)].health > 0.0f) {
                target = i;
                break;
            }
        }
        if (target >= 0) {
            const LevelObject& obj = rig.level.objects[static_cast<size_t>(target)];
            rig.ship.position = obj.position + glm::vec3(0.0f, 0.0f, 16.0f);
            rig.ship.segment = findSegment(rig.level, rig.ship.position, obj.segment);
            // Deliberately aimed past the target, so only the splash can reach.
            rig.aimAt(obj.position + glm::vec3(6.0f, 0.0f, 0.0f));

            sim::FireControls controls;
            controls.selectWeapon = sim::WeaponConcussion;
            controls.secondary = true;
            rig.run(1, controls);
            controls.secondary = false;
            rig.run(240, controls);

            const float remaining = rig.state.objects.health[static_cast<size_t>(target)];
            ok &= expect(remaining < obj.health, "a near miss still damaged the target");
            log::info("splash: near miss took the target from {:.0f} to {:.1f} health", obj.health,
                      remaining);
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

        const int live = rig.state.projectiles.count();
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
