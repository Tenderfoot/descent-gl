#include "sim/combat.h"

#include "core/log.h"
#include "game/level.h"
#include "sim/collision.h"
#include "sim/robots.h"
#include "sim/ship.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace dvk::sim {

namespace {

// Deterministic spread. A std::mt19937 per shot would be both slower and
// harder to reproduce from a bug report; this is a counter the sim owns.
uint32_t g_spreadCounter = 0;

float nextUnit() {
    g_spreadCounter = g_spreadCounter * 1664525u + 1013904223u;
    return static_cast<float>((g_spreadCounter >> 8) & 0xFFFFFFu) / 16777215.0f;
}

glm::vec3 spreadDirection(const glm::vec3& forward, const glm::vec3& right, const glm::vec3& up,
                          float halfAngle) {
    if (halfAngle <= 0.0f) return forward;
    const float a = nextUnit() * 6.2831853f;
    // sqrt so the samples are uniform over the disc rather than clustered at
    // the centre; a cluster reads as the gun being more accurate than it is.
    const float r = std::sqrt(nextUnit()) * halfAngle;
    return glm::normalize(forward + right * (std::cos(a) * r) + up * (std::sin(a) * r));
}

void spawnEffect(CombatState& state, const glm::vec3& position, int segment,
                 const glm::vec3& color, float size, float lifetime, float lightRadius,
                 float lightIntensity) {
    const int i = state.effects.spawn();
    if (i < 0) return;
    Effect& e = state.effects.items[static_cast<size_t>(i)];
    e.position = position;
    e.segment = segment;
    e.color = color;
    e.size = size;
    e.lifetime = lifetime;
    e.lightRadius = lightRadius;
    e.lightIntensity = lightIntensity;
}

// Applies damage to everything within `radius` of `centre`, falling off
// linearly - robots, destructible scenery, and the player. Splash that does not
// hurt the person who fired it is a missile you can safely shoot at your own
// feet, which is not the weapon this is meant to be.
int applySplash(const Level& level, CombatState& state, RobotState* robots, Body* player,
                const glm::vec3& centre, float radius, float damage) {
    if (radius <= 0.0f || damage <= 0.0f) return 0;
    int hits = 0;

    for (int i = 0; i < state.objects.count; ++i) {
        if (!state.objects.alive[static_cast<size_t>(i)]) continue;
        const LevelObject& obj = level.objects[static_cast<size_t>(i)];
        const float d = glm::length(obj.position - centre);
        if (d > radius) continue;
        state.objects.health[static_cast<size_t>(i)] -= damage * (1.0f - d / radius);
        ++hits;
    }

    if (robots) {
        for (int i = 0; i < robots->count; ++i) {
            Robot& robot = robots->robots[static_cast<size_t>(i)];
            if (!robot.alive) continue;
            const float d = glm::length(robot.body.position - centre);
            if (d > radius) continue;
            robot.health -= damage * (1.0f - d / radius);
            ++hits;
        }
    }

    if (player) {
        const float d = glm::length(player->position - centre);
        if (d <= radius) {
            state.player.shields -= damage * (1.0f - d / radius);
            ++hits;
        }
    }
    return hits;
}

// Objects are spheres for hit purposes. Their meshes are small and roughly
// round, and a projectile test against 20 triangles per object per step is not
// worth the accuracy.
constexpr float kObjectRadius = 2.4f;

int hitObject(const Level& level, const CombatState& state, const glm::vec3& from,
              const glm::vec3& to, float radius, glm::vec3* hitPoint) {
    int best = -1;
    float bestT = 2.0f;
    const glm::vec3 seg = to - from;
    const float segLenSq = glm::dot(seg, seg);
    if (segLenSq <= 0.0f) return -1;

    for (int i = 0; i < state.objects.count; ++i) {
        if (!state.objects.alive[static_cast<size_t>(i)]) continue;
        const LevelObject& obj = level.objects[static_cast<size_t>(i)];
        // Closest point on the travel segment to the object's centre.
        const float t = std::clamp(glm::dot(obj.position - from, seg) / segLenSq, 0.0f, 1.0f);
        const glm::vec3 closest = from + seg * t;
        if (glm::length(closest - obj.position) <= kObjectRadius + radius && t < bestT) {
            bestT = t;
            best = i;
            if (hitPoint) *hitPoint = closest;
        }
    }
    return best;
}

void killProjectile(const Level& level, CombatState& state, RobotState* robots, Body* player,
                    Projectile& p, const glm::vec3& at, int segment) {
    p.alive = false;

    if (p.kind == ProjectileKind::Flare) {
        const int i = state.flares.spawn();
        if (i >= 0) {
            Flare& f = state.flares.items[static_cast<size_t>(i)];
            f.position = at;
            f.segment = segment;
            f.lifetime = p.lifetime;
        }
        return;
    }

    const bool explosive = p.splashRadius > 0.0f;
    if (explosive) {
        applySplash(level, state, robots, player, at, p.splashRadius, p.splashDamage);
        spawnEffect(state, at, segment, glm::vec3(1.0f, 0.65f, 0.3f), 6.0f, 0.45f,
                    p.splashRadius * 1.6f, 7.0f);
    } else {
        spawnEffect(state, at, segment, p.color, 1.2f, 0.16f, 9.0f, 2.2f);
    }
}

} // namespace

void initObjects(const Level& level, const RobotConfig& robots, ObjectHealth& out) {
    out.count = std::min(static_cast<int>(level.objects.size()), kMaxObjects);
    for (int i = 0; i < out.count; ++i) {
        const LevelObject& obj = level.objects[static_cast<size_t>(i)];
        out.health[static_cast<size_t>(i)] = obj.health;
        // Health of zero means indestructible scenery, not already dead. A
        // robot's entry is dead from the start: the robot list owns it.
        out.alive[static_cast<size_t>(i)] = !objectIsRobot(robots, obj.type);
    }
}

void stepCombat(const Level& level, CombatState& state, Body& ship,
                const glm::quat& shipOrientation, const WeaponConfig& cfg,
                const FireControls& controls, float dt, RobotState* robots,
                const SolidDoors* doors) {
    PlayerState& player = state.player;

    if (controls.selectWeapon >= 0 && controls.selectWeapon < WeaponCount) {
        player.weapon = controls.selectWeapon;
    }

    for (float& c : player.cooldown) c = std::max(0.0f, c - dt);
    player.flareCooldown = std::max(0.0f, player.flareCooldown - dt);

    // --- resources -------------------------------------------------------
    if (player.headlightOn) {
        player.energy = std::max(0.0f, player.energy - cfg.headlightDrain * dt);
        // Out of energy, the light goes out. In a game with no ambient term
        // that is a real consequence, which is the point of it costing
        // anything at all.
        if (player.energy <= 0.0f) player.headlightOn = false;
    }
    if (cfg.energyRecharge > 0.0f) {
        player.energy = std::min(100.0f, player.energy + cfg.energyRecharge * dt);
    }

    const glm::mat3 basis = glm::mat3_cast(shipOrientation);
    const glm::vec3 forward = basis * glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 right = basis * glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 up = basis * glm::vec3(0.0f, 1.0f, 0.0f);

    // --- firing ----------------------------------------------------------
    const auto fire = [&](int weaponId) {
        const WeaponDef& def = cfg.weapons[static_cast<size_t>(weaponId)];
        if (player.cooldown[static_cast<size_t>(weaponId)] > 0.0f) return;

        float damage = def.damage;
        glm::vec3 color = def.color;
        if (weaponId == WeaponLaser) {
            const int level0 = std::clamp(player.laserLevel, 1, kLaserLevels) - 1;
            damage = cfg.laserDamage[static_cast<size_t>(level0)];
            color = cfg.laserColor[static_cast<size_t>(level0)];
        }

        if (def.energyCost > 0.0f) {
            if (player.energy < def.energyCost) return;
        }
        int* ammo = nullptr;
        if (weaponId == WeaponVulcan) ammo = &player.vulcanAmmo;
        if (weaponId == WeaponConcussion) ammo = &player.concussionAmmo;
        if (weaponId == WeaponHoming) ammo = &player.homingAmmo;
        if (def.ammoCost > 0 && ammo && *ammo < def.ammoCost) return;

        const int slot = state.projectiles.spawn();
        if (slot < 0) return;

        if (def.energyCost > 0.0f) player.energy -= def.energyCost;
        if (def.ammoCost > 0 && ammo) *ammo -= def.ammoCost;
        player.cooldown[static_cast<size_t>(weaponId)] = def.fireInterval;

        // Alternating wingtips, from the two points the ship model's widest
        // vertices sit at. See kShipFirePoints.
        glm::vec3 origin = ship.position;
        if (weaponId == WeaponLaser || weaponId == WeaponVulcan) {
            const glm::vec3 local =
                kShipFirePoints[player.nextWingRight ? 1 : 0];
            origin += basis * local;
            player.nextWingRight = !player.nextWingRight;
        } else {
            origin += forward * 1.5f;
        }

        Projectile& p = state.projectiles.items[static_cast<size_t>(slot)];
        p.kind = static_cast<ProjectileKind>(weaponId);
        p.position = origin;
        p.velocity = spreadDirection(forward, right, up, def.spread) * def.speed + ship.velocity;
        p.color = color;
        p.segment = ship.segment;
        p.radius = def.radius;
        p.damage = damage;
        p.splashRadius = def.splashRadius;
        p.splashDamage = def.splashDamage;
        p.lifetime = def.lifetime;
        p.lightRadius = def.lightRadius;
        p.lightIntensity = def.lightIntensity;
        p.turnRate = def.turnRate;
        p.owner = -1;

        // Homing acquires once, at launch, from whatever is inside its forward
        // cone. Re-acquiring every step makes it feel like a guided camera
        // rather than a missile.
        if (weaponId == WeaponHoming) {
            float bestScore = -1.0f;
            const auto consider = [&](const glm::vec3& position) {
                const glm::vec3 to = position - origin;
                const float dist = glm::length(to);
                if (dist > def.seekRange || dist < 0.001f) return false;
                const float cosAngle = glm::dot(to / dist, forward);
                if (cosAngle < std::cos(def.seekCone)) return false;
                const float score = cosAngle - dist / def.seekRange;
                if (score <= bestScore) return false;
                bestScore = score;
                return true;
            };
            // Robots are what a missile is for. Destructible scenery is a
            // target too, but only if nothing alive scores better.
            if (robots) {
                for (int i = 0; i < robots->count; ++i) {
                    const Robot& robot = robots->robots[static_cast<size_t>(i)];
                    if (!robot.alive) continue;
                    if (consider(robot.body.position)) {
                        p.targetRobot = i;
                        p.target = -1;
                    }
                }
            }
            for (int i = 0; i < state.objects.count; ++i) {
                if (!state.objects.alive[static_cast<size_t>(i)]) continue;
                if (level.objects[static_cast<size_t>(i)].health <= 0.0f) continue;
                if (consider(level.objects[static_cast<size_t>(i)].position)) {
                    p.target = i;
                    p.targetRobot = -1;
                }
            }
        }
    };

    if (controls.primary) {
        if (player.weapon == WeaponLaser || player.weapon == WeaponVulcan) fire(player.weapon);
    }
    if (controls.secondary) {
        fire(player.weapon == WeaponHoming ? WeaponHoming : WeaponConcussion);
    }
    if (controls.flare && player.flareCooldown <= 0.0f && player.flares > 0) {
        const int slot = state.projectiles.spawn();
        if (slot >= 0) {
            --player.flares;
            player.flareCooldown = 0.35f;
            Projectile& p = state.projectiles.items[static_cast<size_t>(slot)];
            p.kind = ProjectileKind::Flare;
            p.position = ship.position + forward * 1.8f;
            // Thrown slightly upward so it arcs away from the nose rather than
            // appearing to fall out of the ship.
            p.velocity = (forward + up * 0.18f) * cfg.flareSpeed + ship.velocity;
            p.color = cfg.flareColor;
            p.segment = ship.segment;
            p.radius = 0.4f;
            p.lifetime = cfg.flareLifetime;
            p.lightRadius = cfg.flareRadius;
            p.lightIntensity = cfg.flareIntensity;
        }
    }

    // --- projectiles -----------------------------------------------------
    for (Projectile& p : state.projectiles.items) {
        if (!p.alive) continue;

        p.age += dt;
        if (p.age >= p.lifetime) {
            // A flare that never landed still becomes a light where it ran out.
            killProjectile(level, state, robots, &ship, p, p.position, p.segment);
            continue;
        }

        // Where a homing missile is steering: a live robot first, else a
        // destructible object. A target that has died is simply flown past.
        const glm::vec3* homingGoal = nullptr;
        if (p.kind == ProjectileKind::Homing) {
            if (robots && p.targetRobot >= 0 && p.targetRobot < robots->count &&
                robots->robots[static_cast<size_t>(p.targetRobot)].alive) {
                homingGoal = &robots->robots[static_cast<size_t>(p.targetRobot)].body.position;
            } else if (p.target >= 0 && state.objects.alive[static_cast<size_t>(p.target)]) {
                homingGoal = &level.objects[static_cast<size_t>(p.target)].position;
            }
        }
        if (homingGoal) {
            const glm::vec3 to = *homingGoal - p.position;
            const float dist = glm::length(to);
            if (dist > 0.001f) {
                const float speed = glm::length(p.velocity);
                const glm::vec3 desired = to / dist;
                const glm::vec3 current = speed > 0.0f ? p.velocity / speed : desired;
                // Capped turn: rotate towards the target by at most turnRate.
                const float cosAngle = std::clamp(glm::dot(current, desired), -1.0f, 1.0f);
                const float angle = std::acos(cosAngle);
                const float maxTurn = p.turnRate * dt;
                const glm::vec3 dir =
                    angle <= maxTurn
                        ? desired
                        : glm::normalize(current + (desired - current) * (maxTurn / angle));
                p.velocity = dir * speed;
            }
        }

        const glm::vec3 from = p.position;
        const glm::vec3 step = p.velocity * dt;
        const float distance = glm::length(step);
        if (distance <= 0.0f) continue;
        const glm::vec3 dir = step / distance;

        // Targets first, then walls: a shot that would pass through a robot on
        // its way to the wall behind it should stop at the robot.
        glm::vec3 objectPoint(0.0f);
        const int object =
            p.kind == ProjectileKind::Flare
                ? -1
                : hitObject(level, state, from, from + step, p.radius, &objectPoint);

        // A robot's own fire never hits a robot, and the player's never hits
        // the player. Friendly fire between robots would turn a corridor of
        // them into a brawl the player watches.
        int robotHit = -1;
        glm::vec3 robotPoint(0.0f);
        if (robots && p.owner < 0 && p.kind != ProjectileKind::Flare) {
            float best = 2.0f;
            const glm::vec3 seg = step;
            const float segLenSq = glm::dot(seg, seg);
            for (int i = 0; i < robots->count && segLenSq > 0.0f; ++i) {
                const Robot& robot = robots->robots[static_cast<size_t>(i)];
                if (!robot.alive) continue;
                const float t =
                    std::clamp(glm::dot(robot.body.position - from, seg) / segLenSq, 0.0f, 1.0f);
                const glm::vec3 closest = from + seg * t;
                if (glm::length(closest - robot.body.position) <= robot.body.radius + p.radius &&
                    t < best) {
                    best = t;
                    robotHit = i;
                    robotPoint = closest;
                }
            }
        }

        bool hitPlayer = false;
        glm::vec3 playerPoint(0.0f);
        if (p.owner >= 0 && p.kind != ProjectileKind::Flare) {
            const glm::vec3 seg = step;
            const float segLenSq = glm::dot(seg, seg);
            if (segLenSq > 0.0f) {
                const float t =
                    std::clamp(glm::dot(ship.position - from, seg) / segLenSq, 0.0f, 1.0f);
                const glm::vec3 closest = from + seg * t;
                if (glm::length(closest - ship.position) <= ship.radius + p.radius) {
                    hitPlayer = true;
                    playerPoint = closest;
                }
            }
        }

        const RayHit wall = sweepSphere(level, from, dir, p.radius, distance, p.segment, doors);

        if (hitPlayer && (!wall.hit || glm::length(playerPoint - from) <= wall.distance)) {
            state.player.shields -= p.damage;
            spawnEffect(state, playerPoint, p.segment, p.color, 1.4f, 0.18f, 8.0f, 2.0f);
            killProjectile(level, state, robots, &ship, p, playerPoint, p.segment);
            continue;
        }

        if (robotHit >= 0 && (!wall.hit || glm::length(robotPoint - from) <= wall.distance)) {
            robots->robots[static_cast<size_t>(robotHit)].health -= p.damage;
            // A hit is also a noise: it wakes the thing you just shot even if
            // it had not seen you.
            robots->robots[static_cast<size_t>(robotHit)].awareness = 1.0f;
            robots->robots[static_cast<size_t>(robotHit)].lastKnown = ship.position;
            robots->robots[static_cast<size_t>(robotHit)].hasLastKnown = true;
            spawnEffect(state, robotPoint, p.segment, p.color, 1.7f, 0.22f, 11.0f, 2.8f);
            killProjectile(level, state, robots, &ship, p, robotPoint, p.segment);
            continue;
        }

        const bool hitObjectFirst =
            object >= 0 && (!wall.hit || glm::length(objectPoint - from) <= wall.distance);

        if (hitObjectFirst) {
            state.objects.health[static_cast<size_t>(object)] -= p.damage;
            spawnEffect(state, objectPoint, p.segment, p.color, 1.6f, 0.2f, 10.0f, 2.6f);
            killProjectile(level, state, robots, &ship, p, objectPoint, p.segment);
            continue;
        }
        if (wall.hit) {
            // Pulled back along the direction of travel so the impact flash and
            // any resulting flare sit in front of the wall rather than inside
            // it, where the light would be occluded by the wall it is lighting.
            const glm::vec3 at = from + dir * std::max(0.0f, wall.distance - p.radius * 0.5f);
            killProjectile(level, state, robots, &ship, p, at,
                           wall.segment >= 0 ? wall.segment : p.segment);
            continue;
        }

        p.position = from + step;
        p.segment = trackSegment(level, p.position, p.segment);
    }

    // --- flares ----------------------------------------------------------
    for (Flare& f : state.flares.items) {
        if (!f.alive) continue;
        f.age += dt;
        if (f.age >= f.lifetime) f.alive = false;
    }

    // --- effects ---------------------------------------------------------
    for (Effect& e : state.effects.items) {
        if (!e.alive) continue;
        e.age += dt;
        if (e.age >= e.lifetime) e.alive = false;
    }

    // --- object deaths ---------------------------------------------------
    for (int i = 0; i < state.objects.count; ++i) {
        if (!state.objects.alive[static_cast<size_t>(i)]) continue;
        const LevelObject& obj = level.objects[static_cast<size_t>(i)];
        if (obj.health <= 0.0f) continue;   // scenery: never dies
        if (state.objects.health[static_cast<size_t>(i)] > 0.0f) continue;

        state.objects.alive[static_cast<size_t>(i)] = false;
        spawnEffect(state, obj.position, obj.segment, glm::vec3(1.0f, 0.75f, 0.4f), 8.0f, 0.6f,
                    30.0f, 8.0f);
    }
}

bool checkPlayerDeath(const Level& level, CombatState& state, Body& ship,
                      glm::quat& shipOrientation) {
    if (state.player.shields > 0.0f) return false;

    ++state.player.deaths;
    spawnEffect(state, ship.position, ship.segment, glm::vec3(1.0f, 0.6f, 0.25f), 9.0f, 0.7f,
                34.0f, 9.0f);

    ship.position = level.start.position;
    ship.segment = level.start.segment;
    ship.velocity = glm::vec3(0.0f);
    shipOrientation = level.start.orientation;

    state.player.shields = 100.0f;
    state.player.energy = 100.0f;
    return true;
}

} // namespace dvk::sim
