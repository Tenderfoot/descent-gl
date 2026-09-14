#include "game/robots_test.h"

#include "core/log.h"
#include "game/level.h"
#include "sim/collision.h"
#include "sim/combat.h"
#include "sim/robots.h"
#include "sim/weapons.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <set>

namespace dvk {

namespace {

constexpr float kStep = 1.0f / 120.0f;

struct Rig {
    Level level;
    sim::WeaponConfig weapons;
    sim::RobotConfig robotCfg;
    sim::RobotState robots;
    sim::CombatState combat;
    sim::Body player;
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};

    void reset() {
        combat = sim::CombatState{};
        sim::initObjects(level, combat.objects);
        sim::spawnRobots(level, robotCfg, robots);
        player = sim::Body{};
        player.radius = 1.6f;
        player.position = level.start.position;
        player.segment = level.start.segment;
        combat.player.headlightOn = false;
    }

    void run(int steps) {
        const sim::FireControls idle;
        for (int i = 0; i < steps; ++i) {
            sim::stepCombat(level, combat, player, orientation, weapons, idle, kStep, &robots);
            sim::stepRobots(level, robotCfg, weapons, robots, combat, player, kStep);
        }
    }

    sim::Robot* firstOfType(const char* name) {
        const int type = robotCfg.indexOf(name);
        for (int i = 0; i < robots.count; ++i) {
            sim::Robot& r = robots.robots[static_cast<size_t>(i)];
            if (r.alive && r.type == type) return &r;
        }
        return nullptr;
    }
};

bool expect(bool condition, const char* what) {
    if (!condition) log::error("FAILED: {}", what);
    return condition;
}

} // namespace

int runRobotsTest(const char* levelPath, const char* weaponConfigPath,
                  const char* robotConfigPath) {
    Rig rig;
    rig.level = loadLevel(levelPath);
    rig.weapons = sim::loadWeaponConfig(weaponConfigPath);
    rig.robotCfg = sim::loadRobotConfig(robotConfigPath);
    bool ok = true;

    ok &= expect(rig.robotCfg.types.size() >= 3, "three robot types are defined");

    // --- a robot that cannot see the player idles --------------------------
    {
        rig.reset();
        // The player start is at one end of the mine; the shaft robots are at
        // the other, around two bends and up a shaft.
        rig.run(240);

        int idle = 0;
        int total = 0;
        for (int i = 0; i < rig.robots.count; ++i) {
            const sim::Robot& r = rig.robots.robots[static_cast<size_t>(i)];
            if (!r.alive) continue;
            ++total;
            if (r.mode == sim::RobotMode::Idle) ++idle;
        }
        ok &= expect(idle > 0, "at least one robot out of sight is idling");
        log::info("idle: {} of {} robots idle two seconds after spawn", idle, total);
    }

    // --- being shot alerts a robot -----------------------------------------
    {
        rig.reset();
        rig.run(120);

        // Deliberately one that is still asleep. Picking "the first drone"
        // found one that could already see the player down the corridor, so
        // the test proved nothing: it was awake before it was shot.
        int slot = -1;
        for (int i = 0; i < rig.robots.count; ++i) {
            const sim::Robot& r = rig.robots.robots[static_cast<size_t>(i)];
            if (r.alive && r.mode == sim::RobotMode::Idle && r.awareness <= 0.0f) {
                slot = i;
                break;
            }
        }
        ok &= expect(slot >= 0, "at least one robot is asleep to be woken");
        if (slot >= 0) {
            const sim::RobotMode before = rig.robots.robots[static_cast<size_t>(slot)].mode;
            const float awarenessBefore =
                rig.robots.robots[static_cast<size_t>(slot)].awareness;

            // A bolt that lands on it, fired from wherever the player is.
            const int p = rig.combat.projectiles.spawn();
            sim::Projectile& bolt = rig.combat.projectiles.items[static_cast<size_t>(p)];
            bolt.kind = sim::ProjectileKind::Laser;
            bolt.position = rig.robots.robots[static_cast<size_t>(slot)].body.position +
                            glm::vec3(0.0f, 0.0f, 4.0f);
            bolt.velocity = glm::vec3(0.0f, 0.0f, -120.0f);
            bolt.segment = rig.robots.robots[static_cast<size_t>(slot)].body.segment;
            bolt.radius = 0.35f;
            bolt.damage = 4.0f;
            bolt.lifetime = 2.0f;
            bolt.owner = -1;

            rig.run(60);
            const sim::Robot& after = rig.robots.robots[static_cast<size_t>(slot)];
            ok &= expect(after.awareness > awarenessBefore || after.mode != before,
                         "being hit woke the robot up");
            log::info("alert: a sleeping robot went {} -> {}, awareness {:.2f}",
                      sim::kRobotModeNames[static_cast<size_t>(before)],
                      sim::kRobotModeNames[static_cast<size_t>(after.mode)], after.awareness);
        }
    }

    // --- pathing through portals --------------------------------------------
    {
        rig.reset();
        // The player stays at the level start, and the robot chosen is
        // whichever live one is physically furthest from it. Picking a segment
        // number instead baked test01's layout into the test, and it stopped
        // finding anything the moment a second level existed.
        sim::Robot* robot = nullptr;
        float furthest = -1.0f;
        for (int i = 0; i < rig.robots.count; ++i) {
            sim::Robot& r = rig.robots.robots[static_cast<size_t>(i)];
            if (!r.alive || r.body.segment == rig.player.segment) continue;
            const float d = glm::length(r.body.position - rig.player.position);
            if (d > furthest) {
                furthest = d;
                robot = &r;
            }
        }
        ok &= expect(robot != nullptr, "a robot starts away from the player");

        if (robot) {
            const int slot = static_cast<int>(robot - rig.robots.robots.data());
            const int startSegment = robot->body.segment;
            // Awareness has to come from somewhere; a hit is the honest way.
            rig.robots.robots[static_cast<size_t>(slot)].awareness = 1.0f;
            rig.robots.robots[static_cast<size_t>(slot)].hasLastKnown = true;
            rig.robots.robots[static_cast<size_t>(slot)].lastKnown = rig.player.position;

            std::set<int> visited;
            for (int i = 0; i < 2400; ++i) {
                // Keep it hunting: without line of sight awareness decays, and
                // this test is about the path, not about perception.
                rig.robots.robots[static_cast<size_t>(slot)].awareness = 1.0f;
                rig.robots.robots[static_cast<size_t>(slot)].lastKnown = rig.player.position;
                rig.run(1);
                if (!rig.robots.robots[static_cast<size_t>(slot)].alive) break;
                visited.insert(rig.robots.robots[static_cast<size_t>(slot)].body.segment);
            }

            const int crossed = static_cast<int>(visited.size()) - 1;
            ok &= expect(crossed >= 2, "the robot crossed at least two portals chasing the player");
            log::info("pathing: robot started in segment {} ({:.0f} units away) and reached {} "
                      "segments ({} portals crossed), ending in segment {}",
                      startSegment, furthest, visited.size(), crossed,
                      rig.robots.robots[static_cast<size_t>(slot)].body.segment);
        }
    }

    // --- robots shoot, and their shots hurt ---------------------------------
    {
        rig.reset();
        sim::Robot* robot = rig.firstOfType("gunner");
        ok &= expect(robot != nullptr, "the level has a gunner");
        if (robot) {
            // Both placed relative to the segment's own centre. Offsetting the
            // player from the ROBOT put it outside the mine, the robot could
            // not see it, and the test failed for a reason that had nothing to
            // do with the AI.
            const glm::vec3 centre = rig.level.segments[static_cast<size_t>(robot->body.segment)].center;
            robot->body.position = centre + glm::vec3(0.0f, 0.0f, -7.0f);
            rig.player.segment = robot->body.segment;
            rig.player.position = centre + glm::vec3(0.0f, 0.0f, 7.0f);

            const float before = rig.combat.player.shields;
            int firedSteps = 0;
            while (rig.combat.player.shields >= before && firedSteps < 1800) {
                rig.run(1);
                ++firedSteps;
            }
            const bool hurt = rig.combat.player.shields < before;
            ok &= expect(hurt, "a gunner's fire damaged the player's shields");
            log::info("attack: shields {:.0f} -> {:.1f} after {:.2f}s at 14 units in the open",
                      before, rig.combat.player.shields, static_cast<float>(firedSteps) * kStep);
        }
    }

    // --- retreat, death, debris and drops ------------------------------------
    {
        int retreated = 0;
        int died = 0;
        int dropped = 0;
        int debrisSeen = 0;

        // Repeated because drops are a dice roll: over enough kills the table
        // has to produce something, and a table that never does is a bug.
        for (int trial = 0; trial < 12; ++trial) {
            rig.reset();
            sim::Robot* robot = rig.firstOfType("drone");
            if (!robot) break;
            const int slot = static_cast<int>(robot - rig.robots.robots.data());
            const sim::RobotDef& def =
                rig.robotCfg.types[static_cast<size_t>(robot->type)];

            const glm::vec3 centre =
                rig.level.segments[static_cast<size_t>(robot->body.segment)].center;
            robot->body.position = centre + glm::vec3(0.0f, 0.0f, -6.0f);
            rig.player.segment = robot->body.segment;
            rig.player.position = centre + glm::vec3(0.0f, 0.0f, 6.0f);

            // Whittle it down and watch for the retreat threshold.
            bool sawRetreat = false;
            for (int i = 0; i < 900; ++i) {
                sim::Robot& r = rig.robots.robots[static_cast<size_t>(slot)];
                if (!r.alive) break;
                if (r.health > def.health * (def.retreatThreshold + 0.05f)) {
                    r.health -= 0.5f;
                }
                rig.run(1);
                if (rig.robots.robots[static_cast<size_t>(slot)].alive &&
                    rig.robots.robots[static_cast<size_t>(slot)].mode == sim::RobotMode::Retreat) {
                    sawRetreat = true;
                }
            }
            if (sawRetreat) ++retreated;

            // Finish it off.
            rig.robots.robots[static_cast<size_t>(slot)].health = -1.0f;
            rig.run(2);
            if (!rig.robots.robots[static_cast<size_t>(slot)].alive) ++died;

            const int debrisNow = static_cast<int>(std::count_if(
                rig.robots.debris.begin(), rig.robots.debris.end(),
                [](const sim::Debris& d) { return d.alive; }));
            debrisSeen = std::max(debrisSeen, debrisNow);

            if (std::any_of(rig.robots.drops.begin(), rig.robots.drops.end(),
                            [](const sim::Drop& d) { return d.alive; })) {
                ++dropped;
            }
        }

        ok &= expect(retreated > 0, "a hurt robot retreated");
        ok &= expect(died == 12, "every robot taken to zero health died");
        ok &= expect(debrisSeen >= 3, "death spawned at least three debris chunks");
        ok &= expect(dropped > 0, "the drop table produced something across twelve kills");
        log::info("death: {}/12 retreated when hurt, {}/12 died, {} debris chunks, "
                  "{}/12 kills dropped something",
                  retreated, died, debrisSeen, dropped);
    }

    // --- robots stay inside the mine ------------------------------------------
    {
        rig.reset();
        // Wake everything and let them all converge on the player for a while.
        for (int i = 0; i < rig.robots.count; ++i) {
            rig.robots.robots[static_cast<size_t>(i)].awareness = 1.0f;
            rig.robots.robots[static_cast<size_t>(i)].hasLastKnown = true;
        }

        int escapes = 0;
        for (int i = 0; i < 1800; ++i) {
            for (int r = 0; r < rig.robots.count; ++r) {
                rig.robots.robots[static_cast<size_t>(r)].awareness = 1.0f;
                rig.robots.robots[static_cast<size_t>(r)].lastKnown = rig.player.position;
            }
            rig.run(1);
            for (int r = 0; r < rig.robots.count; ++r) {
                const sim::Robot& robot = rig.robots.robots[static_cast<size_t>(r)];
                if (!robot.alive) continue;
                if (findSegment(rig.level, robot.body.position, robot.body.segment) < 0) {
                    if (escapes < 4) {
                        log::error("robot {} left the mine at {:.1f} {:.1f} {:.1f}", r,
                                   robot.body.position.x, robot.body.position.y,
                                   robot.body.position.z);
                    }
                    ++escapes;
                }
            }
        }
        ok &= expect(escapes == 0, "no robot left the mine over fifteen seconds of chasing");
        log::info("containment: {} robots chasing for 15 s, {} escapes", rig.robots.aliveCount(),
                  escapes);
    }

    log::info("robots test: {}", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace dvk
