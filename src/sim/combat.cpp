#include "sim/combat.h"

#include "core/log.h"
#include "game/level.h"
#include "sim/collision.h"
#include "sim/props.h"
#include "sim/proxy.h"
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

// How far past a robot's hull a shot may meet its attachment and still count
// as hitting the attachment. One can sit partly inside the hull's sphere, and
// without this a shot aimed squarely at it lands on the robot instead.
constexpr float kAttachmentPreference = 1.5f;

// How much bigger a shootable missile is to the player's fire than it is in
// flight. See traceCombatRay.
constexpr float kShootableMargin = 0.6f;

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
                float playerMass, const SolidDoors* doors, PropState* props) {
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
            robot.lastHitByCharge = false;
            applyImpulse(robot.body, away(robot.body.position), shove, robot.mass);
            ++hits;
        }
    }

    if (props) {
        for (int i = 0; i < props->floatingCount; ++i) {
            FloatingLight& ball = props->floating[static_cast<size_t>(i)];
            if (!ball.alive) continue;
            float d = 0.0f;
            if (!exposed(ball.body.position, &d)) continue;
            applyImpulse(ball.body, away(ball.body.position), share(d).second, ball.mass);
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

// `events` is the sound queue and may be null. Only the explosion is emitted
// here: a bolt landing on a wall, a robot or a crate is a different noise each
// time, and the caller is the only place that knows which of those it was.
void killProjectile(const Level& level, CombatState& state, RobotState* robots, Body* player,
                    Projectile& p, const glm::vec3& at, int segment, float playerMass,
                    const SolidDoors* doors, PropState* props, EventQueue* events) {
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
                    p.splashImpulse, playerMass, doors, props);
        spawnEffect(state, at, segment, glm::vec3(1.0f, 0.65f, 0.3f), 6.0f, 0.45f,
                    p.splashRadius * 1.6f, 7.0f);
        emit(events, Cue::Explosion, at, segment);
    } else {
        spawnEffect(state, at, segment, p.color, 1.2f, 0.16f, 9.0f, 2.2f);
    }
}


// The player's weapon landing on a robot's attachment, when it is the weapon
// that attachment breaks to: knocks it loose and spends the shot on it. The
// hull under it is not hurt, but the robot knows exactly who did it. False for
// every other hit, which the caller treats as an ordinary hit on the robot.
//
// One rule for both kinds of fire, so an attachment that breaks to a missile
// breaks the same way as one that breaks to the railgun. `events` null means
// the hit is silent and unlit - the lightning gun's per-step trace.
bool breakAttachment(const Level& level, CombatState& state, RobotState& robots,
                     const CombatHit& hit, int weaponId, const glm::vec3& push,
                     const glm::vec3& shooter, const glm::vec3& color, const SolidDoors* doors,
                     EventQueue* events) {
    if (!hit.attachment || weaponId < 0) return false;
    Robot& robot = robots.robots[static_cast<size_t>(hit.index)];
    if (!robot.attachmentOn || robot.attachmentBreaksOn != weaponId) return false;

    knockAttachmentLoose(level, robots, hit.index, push, doors);
    state.player.lastTargetRobot = hit.index;
    state.player.targetHudTime = kTargetHudTime;
    robot.awareness = 1.0f;
    robot.lastKnown = shooter;
    robot.hasLastKnown = true;
    if (events) {
        spawnEffect(state, hit.point, hit.segment, color, 3.2f, 0.4f, 18.0f, 5.0f);
        emit(events, Cue::AttachmentBreak, hit.point, hit.segment);
    }
    return true;
}

// A shootable robot missile hit by the player's fire: it goes off where it
// is, with the full blast it would have had on arrival. That blast is
// applySplash's like any other, so it hurts the robots around it - including
// the one that fired it, which is the reward for shooting it down close to
// home - and it hurts the player too if they let it get that close.
void shootDownMissile(const Level& level, CombatState& state, RobotState* robots, Body& ship,
                      int index, float playerMass, const SolidDoors* doors, PropState* props,
                      EventQueue* events) {
    Projectile& missile = state.projectiles.items[static_cast<size_t>(index)];
    if (!missile.alive) return;
    // Without a splash of its own it would vanish with only the flash
    // killProjectile gives a bolt, so it makes the noise itself.
    if (missile.splashRadius <= 0.0f) {
        emit(events, Cue::Explosion, missile.position, missile.segment);
    }
    killProjectile(level, state, robots, &ship, missile, missile.position, missile.segment,
                   playerMass, doors, props, events);
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
                         const CombatTraceParams& params, const SolidDoors* doors,
                         const PropState* props) {
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
        hit.door = wall.door;
    }

    if (!params.hitBodies) return hit;

    CombatHit body;
    float bestBody = wall.hit ? wall.distance : maxDistance;
    bool haveBody = false;

    // Exact ray/sphere entry. Besides placing the flash on the target's surface,
    // this gives body-vs-wall ordering the actual first contact rather than the
    // sphere's centre plane.
    const auto consider = [&](const glm::vec3& centre, float bodyRadius, int bodySegment,
                              CombatHit::Kind kind, int index) {
        const float radius = bodyRadius + params.radius;
        const glm::vec3 fromCentre = origin - centre;
        const float projected = glm::dot(fromCentre, direction);
        const float outside = glm::dot(fromCentre, fromCentre) - radius * radius;
        if (outside > 0.0f && projected > 0.0f) return;
        const float discriminant = projected * projected - outside;
        if (discriminant < 0.0f) return;
        const float along = std::max(0.0f, -projected - std::sqrt(discriminant));
        if (along > maxDistance) return;
        const glm::vec3 contact = origin + direction * along;
        // A body level with the wall still stops the shot; a body level with a
        // body already found does not displace it.
        if (haveBody ? along >= bestBody : along > bestBody) return;
        bestBody = along;
        haveBody = true;
        body.kind = kind;
        body.distance = along;
        body.point = contact;
        body.segment = bodySegment;
        body.index = index;
        body.attachment = false;
    };

    // The same decision for a volume that is not a sphere. Kept beside
    // `consider` rather than folded into it: the sphere path is the one every
    // body takes, it runs per robot per shot, and it should not grow a branch
    // for the handful of objects that are boxes.
    const auto considerProxy = [&](const PlacedProxy& proxy, int bodySegment,
                                   CombatHit::Kind kind, int index) {
        const float along = rayProxyDistance(proxy, origin, direction, params.radius);
        if (along < 0.0f || along > maxDistance) return;
        if (haveBody ? along >= bestBody : along > bestBody) return;
        bestBody = along;
        haveBody = true;
        body.kind = kind;
        body.distance = along;
        body.point = origin + direction * along;
        body.segment = bodySegment;
        body.index = index;
        body.attachment = false;
    };

    // The player, for robot fire. Checked first so that where a robot's bolt
    // reaches the player and a crate at the same instant, the player is hit.
    if (params.owner >= 0) {
        consider(ship.position, ship.radius, ship.segment, CombatHit::Kind::Player, -1);
    }

    // Robots, for the player's fire.
    if (robots && params.owner < 0) {
        for (int i = 0; i < robots->count; ++i) {
            const Robot& robot = robots->robots[static_cast<size_t>(i)];
            if (!robot.alive) continue;
            consider(robot.body.position, robot.body.radius, robot.body.segment,
                     CombatHit::Kind::Robot, i);

            // An attachment is a box of its own. Where it sits inside the
            // hull's sphere a shot aimed at it meets the sphere first, so the
            // attachment wins anything within kAttachmentPreference of the
            // hull: the player hits what they can see they are aiming at.
            if (!robot.attachmentOn) continue;
            const float along =
                rayProxyDistance(robot.attachmentProxy, origin, direction, params.radius);
            if (along < 0.0f || along > maxDistance) continue;
            const bool hullHere = haveBody && body.kind == CombatHit::Kind::Robot && body.index == i;
            const bool wins = hullHere  ? along <= bestBody + kAttachmentPreference
                              : haveBody ? along < bestBody
                                         : along <= bestBody;
            if (!wins) continue;
            bestBody = along;
            haveBody = true;
            body.kind = CombatHit::Kind::Robot;
            body.distance = along;
            body.point = origin + direction * along;
            body.segment = robot.body.segment;
            body.index = i;
            body.attachment = true;
        }
    }

    // Shootable robot missiles, for the player's fire. A little fatter than
    // they fly, because a missile coming at you head-on is a small thing to
    // hit and shooting it down is meant to be an answer, not a trick shot.
    if (params.owner < 0) {
        for (int i = 0; i < kMaxProjectiles; ++i) {
            const Projectile& missile = state.projectiles.items[static_cast<size_t>(i)];
            if (!missile.alive || !missile.shootable || missile.owner < 0) continue;
            consider(missile.position, missile.radius + kShootableMargin, missile.segment,
                     CombatHit::Kind::Missile, i);
        }
    }

    // Destructible scenery, which either side can hit.
    //
    // Against the object's own proxy volume, not a nominal radius. Every object
    // used to be a 2.4 unit sphere on its origin, which for `pillar.obj` - a
    // column 4.4 x 18 x 4.4 - meant a shot anywhere but its middle passed
    // through and hit the wall behind. See `sim::ProxyVolume`.
    for (int i = 0; i < state.objects.count; ++i) {
        if (!state.objects.alive[static_cast<size_t>(i)]) continue;
        if (static_cast<size_t>(i) >= level.objectProxies.size()) continue;
        const LevelObject& object = level.objects[static_cast<size_t>(i)];
        considerProxy(level.objectProxies[static_cast<size_t>(i)], object.segment,
                      CombatHit::Kind::Object, i);
    }

    // Floating lights. They take no damage - shooting one moves it, which is
    // the whole point of it - but they do stop a shot, or you would be hitting
    // the wall behind something you can plainly see.
    if (props) {
        for (int i = 0; i < props->floatingCount; ++i) {
            const FloatingLight& ball = props->floating[static_cast<size_t>(i)];
            if (!ball.alive) continue;
            consider(ball.body.position, ball.body.radius, ball.body.segment,
                     CombatHit::Kind::Prop, i);
        }
    }

    return haveBody ? body : hit;
}

int* weaponAmmoPool(PlayerState& player, int weaponId) {
    switch (static_cast<WeaponId>(weaponId)) {
        case WeaponVulcan: return &player.vulcanAmmo;
        case WeaponConcussion: return &player.concussionAmmo;
        case WeaponHoming: return &player.homingAmmo;
        case WeaponRailgun: return &player.railgunAmmo;
        case WeaponLightning: return &player.lightningAmmo;
        case WeaponLaser:
        case WeaponCount:
            break;
    }
    return nullptr;
}

const int* weaponAmmoPool(const PlayerState& player, int weaponId) {
    return weaponAmmoPool(const_cast<PlayerState&>(player), weaponId);
}

bool weaponReady(const PlayerState& player, const WeaponConfig& cfg, int weaponId, float dt) {
    if (weaponId < 0 || weaponId >= WeaponCount) return false;
    const WeaponDef& def = cfg.weapons[static_cast<size_t>(weaponId)];
    const bool continuous = weaponFire(weaponId) == WeaponFire::Continuous;
    const float meter = continuous ? dt : 1.0f;
    if (def.energyCost > 0.0f && player.energy < def.energyCost * meter) return false;
    const int* ammo = weaponAmmoPool(player, weaponId);
    // A continuous weapon's ammoCost is rounds per second, not rounds per pull:
    // a beam that costs eight a second is perfectly able to fire on the three
    // it has left, for rather less than half a second. What it cannot do is
    // fire on nothing, so what is asked of it is one whole round.
    const int needed = continuous ? 1 : def.ammoCost;
    if (def.ammoCost > 0 && ammo && *ammo < needed) return false;
    return true;
}

void stepCombat(const Level& level, CombatState& state, Body& ship,
                const glm::quat& shipOrientation, const WeaponConfig& cfg,
                const FireControls& controls, float dt, RobotState* robots,
                const SolidDoors* doors, EventQueue* events, PropState* props) {
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
    if (controls.cycleSecondary) {
        // Always a change while there is more than one missile to cycle
        // between, so the cue fires on every press - that click is the only
        // feedback a blind cycle gives before the HUD redraws.
        const int next = nextSecondaryWeapon(player.secondaryWeapon);
        if (next != player.secondaryWeapon) emitLocal(events, Cue::WeaponSelect);
        player.secondaryWeapon = next;
    }

    for (float& c : player.cooldown) c = std::max(0.0f, c - dt);
    player.emptyClickTimer = std::max(0.0f, player.emptyClickTimer - dt);
    player.flareCooldown = std::max(0.0f, player.flareCooldown - dt);
    player.hurtTimer = std::max(0.0f, player.hurtTimer - dt);
    player.recoilTimer = std::max(0.0f, player.recoilTimer - dt);
    player.beamCueTimer = std::max(0.0f, player.beamCueTimer - dt);
    player.targetHudTime = std::max(0.0f, player.targetHudTime - dt);
    if (player.targetHudTime <= 0.0f) player.lastTargetRobot = -1;

    // A disabled ship has no power for anything the player might ask of it:
    // the guns are dead and the headlight goes out. Drifting through a mine
    // with no ambient light and nothing to see by is most of what being
    // disabled means. See playerDisabled().
    const bool disabled = playerDisabled(player);
    if (disabled) player.headlightOn = false;

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

    // The Static Charge builds with the ship's own speed: the velocity channel,
    // which the engines are clamped to, and not knockback, which is the mine
    // throwing the ship around. A disabled ship has no power to store. Charge
    // earned while full is lost rather than banked.
    if (player.lightningAmmo >= kStaticChargeCapacity) {
        player.staticChargeCarry = 0.0f;
    } else if (!disabled && cfg.staticChargeCruiseSpeed > 0.0f) {
        const float pace =
            std::min(1.0f, glm::length(ship.velocity) / cfg.staticChargeCruiseSpeed);
        player.staticChargeCarry += cfg.staticChargeRate * pace * dt;
        const int whole = static_cast<int>(player.staticChargeCarry);
        if (whole > 0) {
            player.lightningAmmo = std::min(kStaticChargeCapacity, player.lightningAmmo + whole);
            player.staticChargeCarry -= static_cast<float>(whole);
        }
    }

    const glm::mat3 basis = glm::mat3_cast(shipOrientation);
    const glm::vec3 forward = basis * glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 right = basis * glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 up = basis * glm::vec3(0.0f, 1.0f, 0.0f);

    // --- firing ----------------------------------------------------------
    const auto fire = [&](int weaponId) {
        const WeaponDef& def = cfg.weapons[static_cast<size_t>(weaponId)];
        const WeaponFire mode = weaponFire(weaponId);

        // A continuous weapon is metered by dt instead of gated by a cooldown:
        // it damages and drains for every step the trigger is down, so its
        // numbers are per second and mean the same thing at any frame rate.
        // Everything else waits out its fire interval and pays per shot.
        const float meter = mode == WeaponFire::Continuous ? dt : 1.0f;
        if (mode != WeaponFire::Continuous &&
            player.cooldown[static_cast<size_t>(weaponId)] > 0.0f) {
            return;
        }

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
        const float energyCost = def.energyCost * meter;
        int* ammo = weaponAmmoPool(player, weaponId);
        // Asked through the shared predicate rather than checked here, so that
        // the strip on the HUD reporting a gun as empty and the gun refusing to
        // fire are the same judgement and not two that agree by habit.
        if (!weaponReady(player, cfg, weaponId, dt)) {
            click();
            return;
        }

        // A projectile weapon with a full pool has nothing to fire and must
        // not be charged for it. A hitscan weapon cannot run out of anything.
        int slot = -1;
        if (mode == WeaponFire::Projectile) {
            slot = state.projectiles.spawn();
            if (slot < 0) return;
        }

        if (def.energyCost > 0.0f) player.energy -= energyCost;
        if (def.ammoCost > 0 && ammo) {
            if (mode == WeaponFire::Continuous) {
                // Per second, paid in whole rounds: the step's fraction goes on
                // the carry and comes off the count as it crosses one. See
                // PlayerState::ammoCarry.
                float& carry = player.ammoCarry[static_cast<size_t>(weaponId)];
                carry += static_cast<float>(def.ammoCost) * dt;
                const float whole = std::floor(carry);
                carry -= whole;
                *ammo = std::max(0, *ammo - static_cast<int>(whole));
            } else {
                *ammo -= def.ammoCost;
            }
        }
        if (mode != WeaponFire::Continuous) {
            player.cooldown[static_cast<size_t>(weaponId)] = def.fireInterval;
        }

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
        const auto muzzleFlash = [&]() {
            spawnEffect(state, origin, ship.segment, color, def.radius * 2.5f, kMuzzleFlashTime,
                        def.lightRadius * 1.5f, def.lightIntensity * 2.5f);
        };

        // At the listener, not at the wingtip it actually came from. See the
        // note on SoundEvent::atListener.
        static constexpr std::array<Cue, WeaponCount> kFireCues{{Cue::LaserFire, Cue::VulcanFire,
                                                                Cue::ConcussionFire,
                                                                Cue::HomingFire, Cue::RailgunFire,
                                                                Cue::LightningFire}};
        // The lightning gun traces 120 times a second and must not be heard or
        // seen to flash 120 times a second. One crackle, one flash and one
        // impact light per fireInterval; the beam itself is drawn every step,
        // so what the player sees is a steady arc that pulses.
        const bool audible =
            mode != WeaponFire::Continuous || player.beamCueTimer <= 0.0f;
        if (audible) {
            if (mode == WeaponFire::Continuous) player.beamCueTimer = def.fireInterval;
            emitLocal(events, kFireCues[static_cast<size_t>(weaponId)]);
        }

        if (audible) muzzleFlash();

        // --- hitscan ------------------------------------------------------
        //
        // No projectile at all: the trace runs now and the beam is only the
        // record of where it went. A railgun modelled as a very fast bolt would
        // still be a bolt, and would still have to be swept step by step
        // against the mine; this is the thing the shared trace exists for.
        if (mode != WeaponFire::Projectile) {
            const glm::vec3 shotDir = spreadDirection(forward, right, up, def.spread);
            CombatTraceParams params;   // a ray, not a sweep: it has no width in flight
            params.owner = -1;
            const CombatHit hit = traceCombatRay(level, state, ship, robots, origin, shotDir,
                                                 def.range, ship.segment, params, doors, props);

            const glm::vec3 end = hit.kind == CombatHit::Kind::None
                                      ? origin + shotDir * def.range
                                      : hit.point;
            const int beamSlot = state.beams.spawn();
            if (beamSlot >= 0) {
                Beam& beam = state.beams.items[static_cast<size_t>(beamSlot)];
                beam.start = origin;
                beam.end = end;
                beam.color = color;
                beam.segment = ship.segment;
                beam.width = def.beamWidth;
                beam.lifetime = def.beamTime;
            }

            // Metered for a continuous weapon, whole for an instant one. The
            // impulse is metered the same way, so the lightning gun pushes
            // steadily rather than flicking its target once a step.
            const float shot = damage * meter;
            const float shove = def.impactImpulse * meter;

            switch (hit.kind) {
                case CombatHit::Kind::Robot: {
                    Robot& robot = robots->robots[static_cast<size_t>(hit.index)];
                    if (breakAttachment(level, state, *robots, hit, weaponId,
                                        shotDir * def.impactImpulse, ship.position, color, doors,
                                        audible ? events : nullptr)) {
                        break;
                    }
                    robot.health -= shot;
                    robot.lastHitByCharge = weaponId == WeaponLightning;
                    player.lastTargetRobot = hit.index;
                    player.targetHudTime = kTargetHudTime;
                    robot.awareness = 1.0f;
                    robot.lastKnown = ship.position;
                    robot.hasLastKnown = true;
                    applyImpulse(robot.body, shotDir, shove, robot.mass);
                    if (audible) {
                        spawnEffect(state, hit.point, hit.segment, color, 1.7f, 0.22f, 11.0f, 2.8f);
                        emit(events, Cue::RobotHit, hit.point, hit.segment);
                    }
                    break;
                }
                case CombatHit::Kind::Object:
                    state.objects.health[static_cast<size_t>(hit.index)] -= shot;
                    if (audible) {
                        spawnEffect(state, hit.point, hit.segment, color, 1.6f, 0.2f, 10.0f, 2.6f);
                        emit(events, Cue::WallImpact, hit.point, hit.segment);
                    }
                    break;
                case CombatHit::Kind::Wall:
                    // A door is a wall you can knock on, and a hitscan shot is
                    // always the player's. No-ops on the mine.
                    state.doorsStruck.note(hit.door);
                    if (audible) {
                        // Half the weapon's own light, in line with what a bolt
                        // impact throws. At full strength a railgun hitting a
                        // wall three metres away whites out the screen.
                        spawnEffect(state, end - shotDir * 0.2f, hit.segment, color, 1.3f, 0.18f,
                                    def.lightRadius * 0.5f, def.lightIntensity * 0.55f);
                        emit(events, Cue::WallImpact, end, hit.segment);
                    }
                    break;
                case CombatHit::Kind::Prop: {
                    FloatingLight& ball = props->floating[static_cast<size_t>(hit.index)];
                    applyImpulse(ball.body, shotDir, def.impactImpulse * meter, ball.mass);
                    if (audible) {
                        spawnEffect(state, hit.point, hit.segment, color, 1.4f, 0.18f, 9.0f, 2.2f);
                        emit(events, Cue::WallImpact, hit.point, hit.segment);
                    }
                    break;
                }
                case CombatHit::Kind::Missile:
                    shootDownMissile(level, state, robots, ship, hit.index, cfg.playerMass, doors,
                                     props, events);
                    break;
                case CombatHit::Kind::Player:
                case CombatHit::Kind::None:
                    break;
            }
            return;
        }

        if (weaponId == WeaponConcussion || weaponId == WeaponHoming) {
            player.recoilTimer = kRecoilTime;
        }

        Projectile& p = state.projectiles.items[static_cast<size_t>(slot)];
        p.kind = projectileFor(weaponId);
        p.weapon = weaponId;
        p.position = origin;
        p.velocity = spreadDirection(forward, right, up, def.spread) * def.speed;
        if (weaponInheritsVelocity(weaponId)) p.velocity += ship.velocity;
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

    if (controls.primary && !disabled) fire(player.primaryWeapon);
    if (controls.secondary && !disabled) fire(player.secondaryWeapon);
    if (controls.flare && !disabled && player.flareCooldown <= 0.0f && player.flares > 0) {
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
            killProjectile(level, state, robots, &ship, p, p.position, p.segment, cfg.playerMass, doors, props,
                               events);
            continue;
        }

        // Where a homing missile is steering: the player, for a robot's; a
        // live robot first, else a destructible object, for the player's. A
        // target that has died is simply flown past.
        const glm::vec3* homingGoal = nullptr;
        if (p.kind == ProjectileKind::Homing) {
            if (p.targetPlayer) {
                homingGoal = &ship.position;
            } else if (robots && p.targetRobot >= 0 && p.targetRobot < robots->count &&
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
                                             p.segment, params, doors, props);

        switch (hit.kind) {
            case CombatHit::Kind::Player:
                state.player.shields -= p.damage;
                state.player.hurtTimer = kHurtFlashTime;
                applyImpulse(ship, dir, p.impactImpulse, cfg.playerMass);
                spawnEffect(state, hit.point, p.segment, p.color, 1.4f, 0.18f, 8.0f, 2.0f);
                emitLocal(events, Cue::PlayerHurt);
                killProjectile(level, state, robots, &ship, p, hit.point, p.segment, cfg.playerMass, doors, props,
                               events);
                continue;

            case CombatHit::Kind::Robot: {
                if (breakAttachment(level, state, *robots, hit, p.weapon, dir * p.impactImpulse,
                                    ship.position, p.color, doors, events)) {
                    // Spent on the attachment, but a missile still goes off:
                    // its splash reaches the robot as any blast would.
                    killProjectile(level, state, robots, &ship, p, hit.point, p.segment,
                                   cfg.playerMass, doors, props, events);
                    continue;
                }
                Robot& robot = robots->robots[static_cast<size_t>(hit.index)];
                robot.health -= p.damage;
                robot.lastHitByCharge = false;
                // Only the player's direct impact acquires a HUD target.
                // applySplash remains intentionally unaware of target state.
                if (p.owner < 0) {
                    state.player.lastTargetRobot = hit.index;
                    state.player.targetHudTime = kTargetHudTime;
                }
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
                killProjectile(level, state, robots, &ship, p, hit.point, p.segment, cfg.playerMass, doors, props,
                               events);
                continue;
            }

            case CombatHit::Kind::Object:
                state.objects.health[static_cast<size_t>(hit.index)] -= p.damage;
                spawnEffect(state, hit.point, p.segment, p.color, 1.6f, 0.2f, 10.0f, 2.6f);
                emit(events, Cue::WallImpact, hit.point, p.segment);
                killProjectile(level, state, robots, &ship, p, hit.point, p.segment, cfg.playerMass, doors, props,
                               events);
                continue;

            case CombatHit::Kind::Wall: {
                // A door is a wall you can knock on - but only the player gets
                // to knock, and only with a weapon. A robot's stray bolt
                // landing on a door it happens to be fighting in front of is
                // not a request to open it, and a mine whose doors flap every
                // time a burst goes wide does not hold still long enough to be
                // read; a flare is thrown to light a corridor, not to ask a
                // door for anything. No-ops on the mine.
                if (p.owner < 0 && p.kind != ProjectileKind::Flare) {
                    state.doorsStruck.note(hit.door);
                }
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
                killProjectile(level, state, robots, &ship, p, at, atSegment, cfg.playerMass, doors, props,
                               events);
                continue;
            }

            case CombatHit::Kind::Prop: {
                // It takes no damage. Shooting a light moves it.
                FloatingLight& ball = props->floating[static_cast<size_t>(hit.index)];
                applyImpulse(ball.body, dir, p.impactImpulse, ball.mass);
                spawnEffect(state, hit.point, p.segment, p.color, 1.4f, 0.18f, 9.0f, 2.2f);
                emit(events, Cue::WallImpact, hit.point, p.segment);
                killProjectile(level, state, robots, &ship, p, hit.point, p.segment,
                               cfg.playerMass, doors, props, events);
                continue;
            }

            case CombatHit::Kind::Missile:
                // Both go off: the missile where it was, and the bolt that
                // hit it at the point it did.
                shootDownMissile(level, state, robots, ship, hit.index, cfg.playerMass, doors,
                                 props, events);
                killProjectile(level, state, robots, &ship, p, hit.point, p.segment,
                               cfg.playerMass, doors, props, events);
                continue;

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

    // --- beams -----------------------------------------------------------
    for (Beam& b : state.beams.items) {
        if (!b.alive) continue;
        b.age += dt;
        if (b.age >= b.lifetime) b.alive = false;
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
    state.player.lastTargetRobot = -1;
    state.player.targetHudTime = 0.0f;
    return true;
}

} // namespace dvk::sim
