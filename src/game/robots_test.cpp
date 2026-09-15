#include "game/robots_test.h"

#include "core/log.h"
#include "game/level.h"
#include "sim/collision.h"
#include "sim/lighting.h"
#include "sim/combat.h"
#include "sim/props.h"
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
    sim::PropState props;
    sim::Body player;
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};

    void reset() {
        combat = sim::CombatState{};
        sim::initObjects(level, robotCfg, combat.objects);
        sim::spawnRobots(level, robotCfg, robots);
        player = sim::Body{};
        player.radius = 1.6f;
        player.position = level.start.position;
        player.segment = level.start.segment;
        combat.player.headlightOn = false;
    }

    void run(int steps) {
        const sim::FireControls idle;
        run(steps, idle);
    }

    void run(int steps, const sim::FireControls& fire) {
        for (int i = 0; i < steps; ++i) {
            sim::stepCombat(level, combat, player, orientation, weapons, fire, kStep, &robots);
            sim::stepRobots(level, robotCfg, weapons, robots, combat, player, kStep, nullptr,
                            nullptr, &props);
        }
    }

    // A robot can no longer pick the player out of an unlit stretch of mine, and
    // several cases below deliberately stand the player in one - they are about
    // what a robot DOES once it is on to the player, not about whether it
    // notices. Turning the headlight on says plainly that the player is not
    // hiding, and exercises the real visibility path rather than reaching past
    // it into the robot's awareness.
    void revealPlayer() { combat.player.headlightOn = true; }

    sim::PlayerVisibility visibility() const {
        return sim::playerVisibility(level, combat, weapons, player.position, player.segment,
                                     nullptr, &props);
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
            rig.revealPlayer();

            const float before = rig.combat.player.shields;
            int firedSteps = 0;
            // A robot's gun flashes like the player's, which in a black
            // corridor is often what tells the player where it is - before the
            // bolt arrives and before the eyes are visible. Caught on the step
            // it fires, because the flash is out again in 0.05 s.
            bool sawMuzzleFlash = false;
            while (rig.combat.player.shields >= before && firedSteps < 1800) {
                rig.run(1);
                ++firedSteps;
                if (sawMuzzleFlash) continue;
                for (const sim::Effect& e : rig.combat.effects.items) {
                    if (!e.alive || e.lightIntensity <= 0.0f) continue;
                    if (glm::length(e.position - robot->body.position) < 4.0f) {
                        sawMuzzleFlash = true;
                        break;
                    }
                }
            }
            const bool hurt = rig.combat.player.shields < before;
            ok &= expect(hurt, "a gunner's fire damaged the player's shields");
            ok &= expect(sawMuzzleFlash, "the gunner's fire threw a muzzle flash");
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
            rig.revealPlayer();

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

    // --- darkness hides the player from robots that have not seen them -------
    //
    // Every case below stands the player somewhere genuinely unlit and asserts
    // that first, so none of them can pass because the hiding spot turned out
    // to be bright.
    {
        rig.reset();

        // A robot whose own segment is dark, with the player a few units away
        // in the same segment so line of sight is never the thing under test.
        int slot = -1;
        for (int i = 0; i < rig.robots.count; ++i) {
            const sim::Robot& r = rig.robots.robots[static_cast<size_t>(i)];
            if (!r.alive) continue;
            const glm::vec3 centre =
                rig.level.segments[static_cast<size_t>(r.body.segment)].center;
            const sim::PlayerVisibility here =
                sim::playerVisibility(rig.level, rig.combat, rig.weapons, centre, r.body.segment);
            if (!here.visible) {
                slot = i;
                break;
            }
        }
        ok &= expect(slot >= 0, "the level has a robot standing in the dark");

        if (slot >= 0) {
            const int segment = rig.robots.robots[static_cast<size_t>(slot)].body.segment;
            const glm::vec3 centre = rig.level.segments[static_cast<size_t>(segment)].center;

            const auto place = [&]() {
                rig.robots.robots[static_cast<size_t>(slot)].body.position =
                    centre + glm::vec3(0.0f, 0.0f, -5.0f);
                rig.robots.robots[static_cast<size_t>(slot)].body.segment = segment;
                rig.robots.robots[static_cast<size_t>(slot)].mode = sim::RobotMode::Idle;
                rig.robots.robots[static_cast<size_t>(slot)].awareness = 0.0f;
                rig.robots.robots[static_cast<size_t>(slot)].hasLastKnown = false;
                rig.player.segment = segment;
                rig.player.position = centre + glm::vec3(0.0f, 0.0f, 5.0f);
            };

            // 1. Unlit, in plain line of sight: the robot never notices.
            place();
            rig.combat.player.headlightOn = false;
            const sim::PlayerVisibility hidden = rig.visibility();
            ok &= expect(!hidden.visible, "the hiding spot really is dark");
            log::info("hiding spot: illumination {:.3f}, visible {}", hidden.illumination,
                      hidden.visible);
            rig.run(240);
            const sim::Robot& unlit = rig.robots.robots[static_cast<size_t>(slot)];
            ok &= expect(unlit.awareness <= 0.0f && unlit.mode == sim::RobotMode::Idle,
                         "a robot in line of sight of an unlit player stays idle");

            // 2. The same robot, same spot, headlight on: it notices at once.
            place();
            rig.revealPlayer();
            ok &= expect(rig.visibility().visible, "the headlight makes the player visible");
            rig.run(240);
            const sim::Robot& lamp = rig.robots.robots[static_cast<size_t>(slot)];
            ok &= expect(lamp.awareness > 0.0f,
                         "the same robot notices a player carrying a headlight");

            // 3. Already on to the player, who then goes dark: it keeps coming.
            place();
            rig.combat.player.headlightOn = false;
            rig.robots.robots[static_cast<size_t>(slot)].awareness = 1.0f;
            rig.robots.robots[static_cast<size_t>(slot)].mode = sim::RobotMode::Chase;
            ok &= expect(!rig.visibility().visible, "the player is dark again");
            rig.run(240);
            const sim::Robot& engaged = rig.robots.robots[static_cast<size_t>(slot)];
            ok &= expect(engaged.awareness > 0.0f && engaged.mode != sim::RobotMode::Idle,
                         "an alerted robot keeps engaging a player who goes dark");

            // 4. Firing gives the position away. No rule says so: the muzzle
            //    flash is a light like any other, and it reaches the player
            //    because the player is holding the gun.
            place();
            rig.combat.player.headlightOn = false;
            rig.combat.player.primaryWeapon = sim::WeaponLaser;
            sim::FireControls trigger;
            trigger.primary = true;
            bool litByOwnFire = false;
            for (int i = 0; i < 240 && !litByOwnFire; ++i) {
                rig.run(1, trigger);
                if (rig.visibility().visible) litByOwnFire = true;
            }
            ok &= expect(litByOwnFire, "firing from the dark lights the player up");
            const sim::Robot& shot = rig.robots.robots[static_cast<size_t>(slot)];
            ok &= expect(shot.awareness > 0.0f, "and the robot notices the player who fired");

            // 5. A hit alerts the robot outright, which is a separate mechanism
            //    from the flash: there is no sniping out of the dark. Worth its
            //    own case because the two have to compose - the darkness rule
            //    gates acquisition by sight, and must not quietly gate this.
            place();
            rig.combat.player.headlightOn = false;
            for (sim::Projectile& p : rig.combat.projectiles.items) p.alive = false;
            for (sim::Effect& e : rig.combat.effects.items) e.alive = false;
            {
                sim::Robot& target = rig.robots.robots[static_cast<size_t>(slot)];
                const int p = rig.combat.projectiles.spawn();
                sim::Projectile& bolt = rig.combat.projectiles.items[static_cast<size_t>(p)];
                bolt.kind = sim::ProjectileKind::Laser;
                bolt.position = target.body.position + glm::vec3(0.0f, 0.0f, 4.0f);
                bolt.velocity = glm::vec3(0.0f, 0.0f, -120.0f);
                bolt.segment = target.body.segment;
                bolt.radius = 0.35f;
                bolt.damage = 4.0f;
                bolt.lifetime = 2.0f;
                bolt.owner = -1;
                // No light on it, so the only thing that can wake the robot is
                // the hit itself.
                bolt.lightRadius = 0.0f;
                bolt.lightIntensity = 0.0f;
            }
            rig.run(60);
            const sim::Robot& hit = rig.robots.robots[static_cast<size_t>(slot)];
            ok &= expect(hit.awareness > 0.0f,
                         "a robot hit from the dark is alerted by the hit itself");
        }
    }

    // --- a floating light gives the player away, and shooting it hides them ---
    //
    // The behavioural half of what --light-test asserts about the meter: a
    // robot standing in the dark has to actually notice a player lit by one of
    // the moving lights, and stop noticing when the light is gone. This is the
    // mechanic the floating light exists for.
    {
        rig.reset();
        rig.combat.player.headlightOn = false;

        int slot = -1;
        for (int i = 0; i < rig.robots.count; ++i) {
            const sim::Robot& r = rig.robots.robots[static_cast<size_t>(i)];
            if (!r.alive) continue;
            const glm::vec3 centre =
                rig.level.segments[static_cast<size_t>(r.body.segment)].center;
            if (!sim::playerVisibility(rig.level, rig.combat, rig.weapons, centre,
                                       r.body.segment).visible) {
                slot = i;
                break;
            }
        }
        ok &= expect(slot >= 0, "the level has a robot standing in the dark");

        if (slot >= 0) {
            // Only this one robot. The other fifteen are awake elsewhere in the
            // mine and their fire is light too, which would decide the case
            // from another room.
            for (int i = 0; i < rig.robots.count; ++i) {
                if (i != slot) rig.robots.robots[static_cast<size_t>(i)].alive = false;
            }
            sim::Robot& robot = rig.robots.robots[static_cast<size_t>(slot)];
            const int segment = robot.body.segment;
            const glm::vec3 centre = rig.level.segments[static_cast<size_t>(segment)].center;

            robot.body.position = centre + glm::vec3(0.0f, 0.0f, -5.0f);
            robot.mode = sim::RobotMode::Idle;
            robot.awareness = 0.0f;
            robot.hasLastKnown = false;
            rig.player.segment = segment;
            rig.player.position = centre + glm::vec3(0.0f, 0.0f, 5.0f);

            ok &= expect(!rig.visibility().visible, "the spot really is dark to begin with");

            // Float a light over the player's head.
            rig.props = sim::PropState{};
            rig.props.floatingCount = 1;
            sim::FloatingLight& ball = rig.props.floating[0];
            ball.alive = true;
            ball.body.position = rig.player.position + glm::vec3(0.0f, 3.0f, 0.0f);
            ball.body.segment = segment;

            const sim::PlayerVisibility underLight = rig.visibility();
            ok &= expect(underLight.visible, "a floating light overhead lights the player up");

            rig.run(120);
            ok &= expect(robot.awareness > 0.0f && robot.mode != sim::RobotMode::Idle,
                         "and the robot in the dark notices them");

            // Take the light away - the game does it by shooting the ball out
            // of the room - and the hiding place comes back.
            //
            // The robot has spent the last second shooting, and its bolts and
            // muzzle flashes are lights playerVisibility counts too, so they
            // have to be gone before the dark can be asserted. That they keep
            // the player lit is the same wiring working, one source over.
            rig.props.floating[0].alive = false;
            rig.combat.projectiles = sim::ProjectilePool{};
            rig.combat.effects = sim::EffectPool{};
            robot.awareness = 0.0f;
            robot.mode = sim::RobotMode::Idle;
            robot.hasLastKnown = false;
            ok &= expect(!rig.visibility().visible, "with the light gone they are hidden again");

            rig.run(120);
            ok &= expect(robot.awareness == 0.0f && robot.mode == sim::RobotMode::Idle,
                         "and the robot goes back to not knowing they are there");

            log::info("floating light: illumination {:.3f} lit, {:.3f} once it is gone",
                      underLight.illumination, rig.visibility().illumination);
        }
    }

    // --- awareness decays, and then the dark works again ----------------------
    //
    // The other half of the mechanic. Going dark does not call a robot off; you
    // have to break line of sight and wait it out. Once you have, the dark hides
    // you from it again exactly as it did before it ever saw you.
    //
    // The robot is pinned in place for this one. Left to itself an alerted robot
    // hunts, regains line of sight and re-acquires - correct behaviour, but it
    // makes the decay impossible to observe. Pinning it lets only awareness
    // move, which is the thing under test.
    {
        rig.reset();

        int slot = -1;
        for (int i = 0; i < rig.robots.count; ++i) {
            const sim::Robot& r = rig.robots.robots[static_cast<size_t>(i)];
            if (!r.alive) continue;
            const glm::vec3 centre =
                rig.level.segments[static_cast<size_t>(r.body.segment)].center;
            const sim::PlayerVisibility here =
                sim::playerVisibility(rig.level, rig.combat, rig.weapons, centre, r.body.segment);
            if (!here.visible) {
                slot = i;
                break;
            }
        }
        ok &= expect(slot >= 0, "a robot standing in the dark to wait out");

        if (slot >= 0) {
            const int segment = rig.robots.robots[static_cast<size_t>(slot)].body.segment;
            const glm::vec3 centre = rig.level.segments[static_cast<size_t>(segment)].center;
            const glm::vec3 post = centre + glm::vec3(0.0f, 0.0f, -5.0f);

            const auto runPinned = [&](int steps) {
                for (int i = 0; i < steps; ++i) {
                    rig.run(1);
                    sim::Robot& r = rig.robots.robots[static_cast<size_t>(slot)];
                    r.body.position = post;
                    r.body.segment = segment;
                    r.body.velocity = glm::vec3(0.0f);
                }
            };

            rig.combat.player.headlightOn = false;
            rig.robots.robots[static_cast<size_t>(slot)].body.position = post;
            rig.robots.robots[static_cast<size_t>(slot)].body.segment = segment;
            rig.robots.robots[static_cast<size_t>(slot)].awareness = 1.0f;
            rig.robots.robots[static_cast<size_t>(slot)].mode = sim::RobotMode::Chase;

            // Somewhere this robot genuinely cannot see. Picked by asking,
            // rather than assumed: the level start looked out of the way and
            // turned out to be straight down a corridor from the robot, which
            // kept it locked on and made the decay impossible to observe.
            int hideSegment = -1;
            for (int si = 0; si < rig.level.segmentCount(); ++si) {
                const glm::vec3 centre = rig.level.segments[static_cast<size_t>(si)].center;
                const glm::vec3 delta = centre - post;
                const float distance = glm::length(delta);
                if (distance < 1e-3f) continue;
                const sim::RayHit hit = sim::raycast(rig.level, post, delta / distance, distance,
                                                     segment);
                if (hit.hit && hit.distance < distance - 0.5f) {
                    hideSegment = si;
                    break;
                }
            }
            ok &= expect(hideSegment >= 0, "somewhere in the mine this robot cannot see");
            if (hideSegment < 0) hideSegment = rig.level.start.segment;
            rig.player.segment = hideSegment;
            rig.player.position = rig.level.segments[static_cast<size_t>(hideSegment)].center;
            const float before = rig.robots.robots[static_cast<size_t>(slot)].awareness;
            runPinned(600);   // five seconds against a three-second decay
            const float after = rig.robots.robots[static_cast<size_t>(slot)].awareness;
            log::info("awareness: {:.2f} -> {:.2f} after five seconds out of sight", before, after);
            ok &= expect(after <= 0.0f, "awareness runs out once the player is out of sight");

            // Back into the same room, still dark, still in plain sight. Having
            // forgotten the player, it cannot pick them out again.
            //
            // Bolts and flashes from the fight elsewhere in the mine are lights
            // like any other, and one still burning next to the player would
            // light them up for reasons that have nothing to do with this test.
            for (sim::Projectile& p : rig.combat.projectiles.items) p.alive = false;
            for (sim::Flare& f : rig.combat.flares.items) f.alive = false;
            for (sim::Effect& e : rig.combat.effects.items) e.alive = false;
            rig.player.segment = segment;
            rig.player.position = centre + glm::vec3(0.0f, 0.0f, 5.0f);
            ok &= expect(!rig.visibility().visible, "the room is still dark");
            runPinned(240);
            const sim::Robot& again = rig.robots.robots[static_cast<size_t>(slot)];
            ok &= expect(again.awareness <= 0.0f && again.mode == sim::RobotMode::Idle,
                         "once it has forgotten you, the dark hides you from it again");
        }
    }

    // --- a robot under 10% health drifts, blind and harmless -------------------
    {
        rig.reset();
        rig.combat.player.headlightOn = true;

        sim::Robot& victim = rig.robots.robots[0];
        const sim::RobotDef& def = rig.robotCfg.types[static_cast<size_t>(victim.type)];

        // Wake it and give it a push, so it has momentum to keep.
        victim.awareness = 1.0f;
        victim.lastKnown = rig.player.position;
        victim.hasLastKnown = true;
        rig.run(60);

        // Down to 9%: under the threshold, still alive.
        victim.health = def.health * 0.09f;
        victim.body.velocity = glm::vec3(6.0f, 0.0f, 0.0f);
        const glm::quat attitudeBefore = victim.orientation;
        const int boltsBefore = rig.combat.projectiles.count();
        rig.run(1);

        ok &= expect(victim.mode == sim::RobotMode::Disabled, "9% health disables the robot");
        ok &= expect(victim.awareness == 0.0f, "a disabled robot stops paying attention");

        // Two seconds of drifting, with the player parked in front of it.
        const glm::vec3 driftFrom = victim.body.position;
        const glm::vec3 keptVelocity = victim.body.velocity;
        rig.player.position = victim.body.position + glm::vec3(0.0f, 0.0f, 8.0f);
        rig.run(240);

        ok &= expect(victim.alive, "it is disabled, not destroyed");
        ok &= expect(rig.combat.projectiles.count() <= boltsBefore,
                     "a disabled robot does not shoot");
        ok &= expect(glm::length(victim.body.position - driftFrom) > 0.5f,
                     "a disabled robot keeps moving");
        ok &= expect(victim.orientation != attitudeBefore, "a disabled robot tumbles");
        ok &= expect(findSegment(rig.level, victim.body.position, victim.body.segment) >= 0,
                     "a disabled robot stays inside the mine");

        // It keeps what it had rather than coasting to a stop: only a wall may
        // take it away.
        const float speedNow = glm::length(victim.body.velocity);
        log::info("disabled: drifted {:.1f}u in 2.0s, {:.1f} u/s of {:.1f} kept",
                  glm::length(victim.body.position - driftFrom), speedNow,
                  glm::length(keptVelocity));

        // And it can still be finished off.
        victim.health = -1.0f;
        rig.run(1);
        ok &= expect(!victim.alive, "a disabled robot can still be destroyed");
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
