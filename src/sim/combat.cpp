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

// See the muzzle flash comment in fire(): short, but not one step.
constexpr float kMuzzleFlashTime = 0.05f;

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

}   // anonymous namespace

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

namespace {

// Applies damage to everything within `radius` of `centre`, falling off
// linearly - robots, destructible scenery, and the player. Splash that does not
// hurt the person who fired it is a missile you can safely shoot at your own
// feet, which is not the weapon this is meant to be.
int applySplash(const Level& level, CombatState& state, RobotState* robots, Body* player,
                const glm::vec3& centre, int segment, float radius, float damage, float impulse,
                float playerMass, const SolidDoors* doors) {
    if (radius <= 0.0f || (damage <= 0.0f && impulse <= 0.0f)) return 0;
    int hits = 0;

    // A blast does not reach through rock. Cast from the explosion outward
    // rather than the other way around, so the walk starts in the segment the
    // explosion already knows it is in. The half-unit slack is the same one the
    // robots' line of sight uses: a ray that stops on the target's own skin has
    // not been blocked.
    const auto exposed = [&](const glm::vec3& target, float* distance) {
        const glm::vec3 to = target - centre;
        const float d = glm::length(to);
        *distance = d;
        if (d > radius) return false;
        if (d < 0.001f) return true;
        const RayHit blocked = raycast(level, centre, to / d, d, segment, doors);
        return !blocked.hit || blocked.distance >= d - 0.5f;
    };

    // Damage falls off linearly; the shove falls off squared, so the edge of a
    // blast rocks you and the centre of one throws you. A linear shove reads as
    // a wall of force with a hard rim.
    const auto share = [&](float d) {
        const float t = d / radius;
        return std::pair<float, float>{damage * (1.0f - t), impulse * (1.0f - t) * (1.0f - t)};
    };

    // Everything inside the blast wants a direction to travel; a body exactly
    // at the centre has none, so it goes up.
    const auto away = [&](const glm::vec3& at) {
        const glm::vec3 to = at - centre;
        return glm::dot(to, to) > 1e-6f ? to : glm::vec3(0.0f, 1.0f, 0.0f);
    };

    for (int i = 0; i < state.objects.count; ++i) {
        if (!state.objects.alive[static_cast<size_t>(i)]) continue;
        const LevelObject& obj = level.objects[static_cast<size_t>(i)];
        float d = 0.0f;
        if (!exposed(obj.position, &d)) continue;
        // Scenery does not move: it has no Body to shove.
        state.objects.health[static_cast<size_t>(i)] -= share(d).first;
        ++hits;
    }

    if (robots) {
        for (int i = 0; i < robots->count; ++i) {
            Robot& robot = robots->robots[static_cast<size_t>(i)];
            if (!robot.alive) continue;
            float d = 0.0f;
            if (!exposed(robot.body.position, &d)) continue;
            const auto [hurt, shove] = share(d);
            robot.health -= hurt;
            applyImpulse(robot.body, away(robot.body.position), shove, robot.mass);
            ++hits;
        }
    }

    if (player) {
        float d = 0.0f;
        if (exposed(player->position, &d)) {
            const auto [hurt, shove] = share(d);
            state.player.shields -= hurt;
            if (hurt > 0.0f) state.player.hurtTimer = kHurtFlashTime;
            applyImpulse(*player, away(player->position), shove, playerMass);
            ++hits;
        }
    }
    return hits;
}

// Objects are spheres for hit purposes. Their meshes are small and roughly
// round, and a projectile test against 20 triangles per object per step is not
// worth the accuracy.
constexpr float kObjectRadius = 2.4f;

// `events` is the sound queue and may be null. Only the explosion is emitted
// here: a bolt landing on a wall, a robot or a crate is a different noise each
// time, and the caller is the only place that knows which of those it was.
void killProjectile(const Level& level, CombatState& state, RobotState* robots, Body* player,
                    Projectile& p, const glm::vec3& at, int segment, float playerMass,
                    const SolidDoors* doors, EventQueue* events) {
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
        applySplash(level, state, robots, player, at, segment, p.splashRadius, p.splashDamage,
                    p.splashImpulse, playerMass, doors);
        spawnEffect(state, at, segment, glm::vec3(1.0f, 0.65f, 0.3f), 6.0f, 0.45f,
                    p.splashRadius * 1.6f, 7.0f);
        emit(events, Cue::Explosion, at, segment);
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

CombatHit traceCombatRay(const Level& level, const CombatState& state, const Body& ship,
                         const RobotState* robots, const glm::vec3& origin,
                         const glm::vec3& direction, float maxDistance, int segment,
                         const CombatTraceParams& params, const SolidDoors* doors) {
    CombatHit hit;
    if (maxDistance <= 0.0f) return hit;

    // Geometry first, so every body below only has to beat one number. The
    // sweep is what stops a fast bolt from crossing a thin wall between steps;
    // a ray is enough for anything instantaneous.
    const RayHit wall =
        params.radius > 0.0f
            ? sweepSphere(level, origin, direction, params.radius, maxDistance, segment, doors)
            : raycast(level, origin, direction, maxDistance, segment, doors);
    if (wall.hit) {
        hit.kind = CombatHit::Kind::Wall;
        hit.distance = wall.distance;
        hit.point = origin + direction * wall.distance;
        hit.normal = wall.normal;
        hit.segment = wall.segment >= 0 ? wall.segment : segment;
    }

    if (!params.hitBodies) return hit;

    CombatHit body;
    float bestBody = wall.hit ? wall.distance : maxDistance;
    bool haveBody = false;

    // Closest approach of the shot's centre line to a sphere, compared against
    // the summed radii rather than solved exactly. At these speeds and sizes
    // the difference is well below the radius itself.
    const auto consider = [&](const glm::vec3& centre, float bodyRadius, CombatHit::Kind kind,
                              int index) {
        const float along = std::clamp(glm::dot(centre - origin, direction), 0.0f, maxDistance);
        const glm::vec3 closest = origin + direction * along;
        if (glm::length(closest - centre) > bodyRadius + params.radius) return;
        // A body level with the wall still stops the shot; a body level with a
        // body already found does not displace it.
        if (haveBody ? along >= bestBody : along > bestBody) return;
        bestBody = along;
        haveBody = true;
        body.kind = kind;
        body.distance = along;
        body.point = closest;
        body.segment = segment;
        body.index = index;
    };

    // The player, for robot fire. Checked first so that where a robot's bolt
    // reaches the player and a crate at the same instant, the player is hit.
    if (params.owner >= 0) consider(ship.position, ship.radius, CombatHit::Kind::Player, -1);

    // Robots, for the player's fire.
    if (robots && params.owner < 0) {
        for (int i = 0; i < robots->count; ++i) {
            const Robot& robot = robots->robots[static_cast<size_t>(i)];
            if (!robot.alive) continue;
            consider(robot.body.position, robot.body.radius, CombatHit::Kind::Robot, i);
        }
    }

    // Destructible scenery, which either side can hit.
    for (int i = 0; i < state.objects.count; ++i) {
        if (!state.objects.alive[static_cast<size_t>(i)]) continue;
        consider(level.objects[static_cast<size_t>(i)].position, kObjectRadius,
                 CombatHit::Kind::Object, i);
    }

    return haveBody ? body : hit;
}

void stepCombat(const Level& level, CombatState& state, Body& ship,
                const glm::quat& shipOrientation, const WeaponConfig& cfg,
                const FireControls& controls, float dt, RobotState* robots,
                const SolidDoors* doors, EventQueue* events) {
    PlayerState& player = state.player;

    if (controls.selectWeapon >= 0 && controls.selectWeapon < WeaponCount) {
        int& selected = weaponSlot(controls.selectWeapon) == WeaponSlot::Primary
                            ? player.primaryWeapon
                            : player.secondaryWeapon;
        // Only on a real change: holding a number key down would otherwise
        // re-select the same gun every step and chatter.
        if (controls.selectWeapon != selected) emitLocal(events, Cue::WeaponSelect);
        selected = controls.selectWeapon;
    }

    for (float& c : player.cooldown) c = std::max(0.0f, c - dt);
    player.emptyClickTimer = std::max(0.0f, player.emptyClickTimer - dt);
    player.flareCooldown = std::max(0.0f, player.flareCooldown - dt);
    player.hurtTimer = std::max(0.0f, player.hurtTimer - dt);
    player.recoilTimer = std::max(0.0f, player.recoilTimer - dt);

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

        // Pulling the trigger on an empty gun is information the player needs,
        // and in a mine with no ambient light it is often the only way they
        // find out. Rate limited: the trigger is read every one of the 120
        // steps a second.
        const auto click = [&]() {
            if (player.emptyClickTimer > 0.0f) return;
            player.emptyClickTimer = kEmptyClickInterval;
            emitLocal(events, Cue::WeaponEmpty);
        };
        if (def.energyCost > 0.0f) {
            if (player.energy < def.energyCost) {
                click();
                return;
            }
        }
        int* ammo = nullptr;
        if (weaponId == WeaponVulcan) ammo = &player.vulcanAmmo;
        if (weaponId == WeaponConcussion) ammo = &player.concussionAmmo;
        if (weaponId == WeaponHoming) ammo = &player.homingAmmo;
        if (def.ammoCost > 0 && ammo && *ammo < def.ammoCost) {
            click();
            return;
        }

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

        // The muzzle flash. Scaled from the weapon's own bolt light rather than
        // from a constant, so a vulcan pop is small and a missile launch throws
        // real light down the corridor - and so a weapon retuned in
        // weapons.json brings its flash with it.
        //
        // Short rather than literally one frame: at 120 Hz a single step is 8
        // ms, which at any real frame rate is as likely to be missed entirely
        // as seen. This is long enough to land on a frame and still be gone
        // before it reads as a light source.
        spawnEffect(state, origin, ship.segment, color, def.radius * 2.5f, kMuzzleFlashTime,
                    def.lightRadius * 1.5f, def.lightIntensity * 2.5f);

        // At the listener, not at the wingtip it actually came from. See the
        // note on SoundEvent::atListener.
        static constexpr std::array<Cue, WeaponCount> kFireCues{
            {Cue::LaserFire, Cue::VulcanFire, Cue::ConcussionFire, Cue::HomingFire}};
        emitLocal(events, kFireCues[static_cast<size_t>(weaponId)]);

        if (weaponId == WeaponConcussion || weaponId == WeaponHoming) {
            player.recoilTimer = kRecoilTime;
        }

        Projectile& p = state.projectiles.items[static_cast<size_t>(slot)];
        p.kind = projectileFor(weaponId);
        p.position = origin;
        p.velocity = spreadDirection(forward, right, up, def.spread) * def.speed + ship.velocity;
        p.color = color;
        p.segment = ship.segment;
        p.radius = def.radius;
        p.damage = damage;
        p.splashRadius = def.splashRadius;
        p.splashDamage = def.splashDamage;
        p.impactImpulse = def.impactImpulse;
        p.splashImpulse = def.splashImpulse;
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

    if (controls.primary) fire(player.primaryWeapon);
    if (controls.secondary) fire(player.secondaryWeapon);
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
            emitLocal(events, Cue::FlareThrow);
        }
    }

    // --- projectiles -----------------------------------------------------
    for (Projectile& p : state.projectiles.items) {
        if (!p.alive) continue;

        p.age += dt;
        if (p.age >= p.lifetime) {
            // A flare that never landed still becomes a light where it ran out.
            killProjectile(level, state, robots, &ship, p, p.position, p.segment, cfg.playerMass, doors,
                               events);
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

        // One question, asked once: what does this bolt reach first? Targets
        // beat the wall behind them, and the nearest target wins.
        CombatTraceParams params;
        params.radius = p.radius;
        params.owner = p.owner;
        params.hitBodies = p.kind != ProjectileKind::Flare;
        const CombatHit hit = traceCombatRay(level, state, ship, robots, from, dir, distance,
                                             p.segment, params, doors);

        switch (hit.kind) {
            case CombatHit::Kind::Player:
                state.player.shields -= p.damage;
                state.player.hurtTimer = kHurtFlashTime;
                applyImpulse(ship, dir, p.impactImpulse, cfg.playerMass);
                spawnEffect(state, hit.point, p.segment, p.color, 1.4f, 0.18f, 8.0f, 2.0f);
                emitLocal(events, Cue::PlayerHurt);
                killProjectile(level, state, robots, &ship, p, hit.point, p.segment, cfg.playerMass, doors,
                               events);
                continue;

            case CombatHit::Kind::Robot: {
                Robot& robot = robots->robots[static_cast<size_t>(hit.index)];
                robot.health -= p.damage;
                // Along the line of travel, not away from the impact point: a
                // bullet pushes what it hits the way it was already going.
                applyImpulse(robot.body, dir, p.impactImpulse, robot.mass);
                // A hit is also a noise: it wakes the thing you just shot even
                // if it had not seen you.
                robot.awareness = 1.0f;
                robot.lastKnown = ship.position;
                robot.hasLastKnown = true;
                spawnEffect(state, hit.point, p.segment, p.color, 1.7f, 0.22f, 11.0f, 2.8f);
                emit(events, Cue::RobotHit, hit.point, p.segment);
                killProjectile(level, state, robots, &ship, p, hit.point, p.segment, cfg.playerMass, doors,
                               events);
                continue;
            }

            case CombatHit::Kind::Object:
                state.objects.health[static_cast<size_t>(hit.index)] -= p.damage;
                spawnEffect(state, hit.point, p.segment, p.color, 1.6f, 0.2f, 10.0f, 2.6f);
                emit(events, Cue::WallImpact, hit.point, p.segment);
                killProjectile(level, state, robots, &ship, p, hit.point, p.segment, cfg.playerMass, doors,
                               events);
                continue;

            case CombatHit::Kind::Wall: {
                // Pulled back along the direction of travel so the impact flash
                // and any resulting flare sit in front of the wall rather than
                // inside it, where the light would be occluded by the wall it
                // is lighting.
                const glm::vec3 at =
                    from + dir * std::max(0.0f, hit.distance - p.radius * 0.5f);
                const int atSegment = hit.segment >= 0 ? hit.segment : p.segment;
                // A flare landing is not an impact: it has already made its
                // noise being thrown, and the thing it does next is glow.
                if (p.kind != ProjectileKind::Flare) emit(events, Cue::WallImpact, at, atSegment);
                killProjectile(level, state, robots, &ship, p, at, atSegment, cfg.playerMass, doors,
                               events);
                continue;
            }

            case CombatHit::Kind::None:
                break;
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
        emit(events, Cue::ObjectDestroyed, obj.position, obj.segment);
    }
}

bool checkPlayerDeath(const Level& level, CombatState& state, Body& ship,
                      glm::quat& shipOrientation, EventQueue* events) {
    if (state.player.shields > 0.0f) return false;

    ++state.player.deaths;
    spawnEffect(state, ship.position, ship.segment, glm::vec3(1.0f, 0.6f, 0.25f), 9.0f, 0.7f,
                34.0f, 9.0f);
    emitLocal(events, Cue::PlayerDeath);

    ship.position = level.start.position;
    ship.segment = level.start.segment;
    ship.velocity = glm::vec3(0.0f);
    shipOrientation = level.start.orientation;

    state.player.shields = 100.0f;
    state.player.energy = 100.0f;
    return true;
}

} // namespace dvk::sim
