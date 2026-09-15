#pragma once

#include "sim/body.h"
#include "sim/events.h"
#include "sim/weapons.h"

#include <array>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

namespace dvk {
struct Level;
}

namespace dvk::sim {

// Everything that flies, sticks, burns or explodes.
//
// All three pools are fixed-capacity arrays held by value in SimState, because
// SimState is copied every step and a std::vector there would allocate in the
// simulation loop. A full pool drops the newest request rather than growing;
// at these capacities that only happens under sustained vulcan fire, where one
// missing round is invisible.

enum class ProjectileKind : uint8_t { Laser, Vulcan, Concussion, Homing, Flare };

// Which projectile a weapon launches.
//
// Written out rather than cast from the weapon id. The two enums only look
// parallel: Flare sits at index 4 and has no weapon behind it at all, so a cast
// silently mislabels the first weapon added after Homing - and a hitscan weapon
// launches nothing, which no cast can express.
constexpr ProjectileKind projectileFor(int weaponId) {
    switch (static_cast<WeaponId>(weaponId)) {
        case WeaponLaser:
            return ProjectileKind::Laser;
        case WeaponVulcan:
            return ProjectileKind::Vulcan;
        case WeaponConcussion:
            return ProjectileKind::Concussion;
        case WeaponHoming:
            return ProjectileKind::Homing;
        case WeaponRailgun:
        case WeaponLightning:
            // Hitscan: launches nothing at all. Never reached, because the
            // firing path branches on weaponFire() before it asks - the cases
            // are here so the next weapon cannot slip past this switch, which
            // is the whole reason this is not a cast.
            break;
        case WeaponCount:
            break;
    }
    return ProjectileKind::Laser;
}

struct Projectile {
    bool alive = false;
    ProjectileKind kind = ProjectileKind::Laser;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 color{1.0f};
    int segment = 0;
    float radius = 0.3f;
    float damage = 0.0f;
    float splashRadius = 0.0f;
    float splashDamage = 0.0f;
    float impactImpulse = 0.0f;
    float splashImpulse = 0.0f;
    float age = 0.0f;
    float lifetime = 2.0f;
    float lightRadius = 0.0f;
    float lightIntensity = 0.0f;
    float turnRate = 0.0f;
    int target = -1;       // homing only: a destructible level object
    int targetRobot = -1;  // homing only: a robot, preferred over `target`
    int owner = -1;        // -1 is the player
};

// A flare that has landed and is now a light.
struct Flare {
    bool alive = false;
    glm::vec3 position{0.0f};
    int segment = 0;
    float age = 0.0f;
    float lifetime = 30.0f;
};

// A short-lived additive billboard: an impact flash or an explosion. Not a
// particle system, deliberately - the plan's scope stops here.
struct Effect {
    bool alive = false;
    glm::vec3 position{0.0f};
    glm::vec3 color{1.0f};
    int segment = 0;
    float age = 0.0f;
    float lifetime = 0.35f;
    float size = 1.5f;
    // Bright one-frame-ish lights read as an explosion far better than the
    // billboard does; this is how much light it throws.
    float lightRadius = 0.0f;
    float lightIntensity = 0.0f;
};

// A hitscan tracer: the line a shot took, drawn for a moment after the damage
// has already been dealt. Held by value like every other pool, because SimState
// is copied each step.
struct Beam {
    bool alive = false;
    glm::vec3 start{0.0f};
    glm::vec3 end{0.0f};
    glm::vec3 color{1.0f};
    int segment = 0;
    float width = 0.12f;
    float age = 0.0f;
    float lifetime = 0.10f;
};

inline constexpr int kMaxProjectiles = 128;
inline constexpr int kMaxBeams = 16;
inline constexpr int kMaxFlares = 16;
inline constexpr int kMaxEffects = 48;
inline constexpr int kMaxObjects = 64;

template <class T, int N>
struct Pool {
    std::array<T, N> items{};
    int spawn() {
        for (int i = 0; i < N; ++i) {
            if (!items[static_cast<size_t>(i)].alive) {
                items[static_cast<size_t>(i)] = T{};
                items[static_cast<size_t>(i)].alive = true;
                return i;
            }
        }
        return -1;   // full: the newest request is dropped
    }
    int count() const {
        int n = 0;
        for (const T& item : items) {
            if (item.alive) ++n;
        }
        return n;
    }
};

using ProjectilePool = Pool<Projectile, kMaxProjectiles>;
using FlarePool = Pool<Flare, kMaxFlares>;
using EffectPool = Pool<Effect, kMaxEffects>;
using BeamPool = Pool<Beam, kMaxBeams>;

// The player's resources and loadout.
struct PlayerState {
    float shields = 100.0f;
    float energy = 100.0f;
    int primaryWeapon = WeaponLaser;
    int secondaryWeapon = WeaponConcussion;
    int laserLevel = 1;                 // 1 .. kLaserLevels
    int vulcanAmmo = 2000;
    int concussionAmmo = 8;
    int homingAmmo = 6;
    int flares = 12;
    std::array<float, WeaponCount> cooldown{};
    float flareCooldown = 0.0f;
    // Laser alternates wingtips. One bit of state that makes the weapon read
    // as a pair of guns rather than one.
    bool nextWingRight = false;
    // Off at the start. The mine opening in darkness, with H the first thing
    // the player reaches for, is the introduction the lighting model deserves;
    // starting lit gives that away before the level has said anything.
    bool headlightOn = false;
    int deaths = 0;

    // Feel, not state anything reads back. Both decay in stepCombat and both
    // only ever leave the simulation through RenderState and HudData: the
    // screen flinches, the ship does not.
    float hurtTimer = 0.0f;     // seconds since the hull was last hit
    float recoilTimer = 0.0f;   // seconds since a missile shoved the ship

    // Rate limit for the lightning gun's crackle. Its trace runs every one of
    // the 120 steps a second; its sound must not.
    float beamCueTimer = 0.0f;

    // Rate limit for the out-of-ammo click. The trigger is read every step, so
    // without this an empty gun clicks 120 times a second, which sounds like a
    // fault in the program rather than like an empty gun.
    float emptyClickTimer = 0.0f;
};

// Full shields. The number the disabled threshold is a fraction of.
inline constexpr float kPlayerMaxShields = 100.0f;

// True while the ship is too badly damaged to fly itself. At or below this the
// player has no thrust, no steering, no guns and no headlight: the ship drifts
// on what it had. Robots use the same fraction - see kDisabledHealthFraction.
inline bool playerDisabled(const PlayerState& player) {
    return player.shields <= kPlayerMaxShields * kDisabledHealthFraction;
}

// How long after an out-of-ammo click before another one is allowed.
inline constexpr float kEmptyClickInterval = 0.4f;

// How long the hit tint and the missile kick take to fade out.
inline constexpr float kHurtFlashTime = 0.45f;
inline constexpr float kRecoilTime = 0.18f;

// Health of each level object, indexed the same as Level::objects. Zero or
// below is destroyed.
struct ObjectHealth {
    std::array<float, kMaxObjects> health{};
    std::array<bool, kMaxObjects> alive{};
    int count = 0;
};

// What the player is asking of the weapons this step.
struct FireControls {
    bool primary = false;      // left mouse
    bool secondary = false;    // right mouse
    bool flare = false;
    // 0..1 select the primary weapon, 2..3 select the secondary weapon.
    int selectWeapon = -1;
};

struct CombatState {
    ProjectilePool projectiles;
    FlarePool flares;
    EffectPool effects;
    BeamPool beams;
    PlayerState player;
    ObjectHealth objects;
};

struct RobotState;
struct RobotConfig;
struct SolidDoors;
struct PropState;

// --- the combat trace ----------------------------------------------------

// What a shot ran into first. Kind::None means it reached its full length
// without touching anything.
struct CombatHit {
    enum class Kind : uint8_t { None, Wall, Robot, Object, Player, Prop };

    Kind kind = Kind::None;
    float distance = 0.0f;
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f};   // walls and doors only, out of the surface
    int segment = -1;         // the segment the hit is in
    int index = -1;           // robot slot for Robot, Level::objects index for Object,
                              // floating-light slot for Prop
};

struct CombatTraceParams {
    // Above zero sweeps a sphere of this radius instead of casting a ray, so a
    // fast projectile cannot pass through a thin wall between two steps.
    float radius = 0.0f;
    // Who fired: -1 is the player, anything else is that robot's slot. A shot
    // never hits its own side - the player's fire passes through the player,
    // and no robot's fire touches any robot, because a corridor of them
    // shooting each other is a brawl the player only watches.
    int owner = -1;
    // False traces geometry alone. Flares are thrown, not fired: they land on
    // the floor rather than sticking into whatever they crossed.
    bool hitBodies = true;
};

// The nearest of wall, shut door, robot, destructible object and player along
// `direction` from `origin`, walking the segment graph as it goes.
//
// This is the one place that answers "what is in front of me". The projectile
// loop, the hitscan weapons and any future robot hitscan all ask it, rather
// than each growing its own copy of the sphere test - which is how the three
// copies this replaced came to disagree about which target wins a tie.
CombatHit traceCombatRay(const Level& level, const CombatState& state, const Body& ship,
                         const RobotState* robots, const glm::vec3& origin,
                         const glm::vec3& direction, float maxDistance, int segment,
                         const CombatTraceParams& params = {}, const SolidDoors* doors = nullptr,
                         const PropState* props = nullptr);

// Sets up object health from the level's objects. An object whose type names
// a robot definition is not scenery: the simulation owns it as a robot, so it
// is marked dead here and never acts as a target. Before this, every robot's
// spawn point was an invisible, indestructible sphere that stopped shots.
void initObjects(const Level& level, const RobotConfig& robots, ObjectHealth& out);

// One fixed step of everything in this file. `ship` is the player's body, used
// for fire points, as a target for robot fire, and for taking splash damage.
// `robots` may be null before M6's robots exist. `doors` is the shut-door set
// for this step; a bolt stops at a shut door as it does at a wall.
// `events` collects what happened for the audio layer and may be null: the
// headless tests drive the simulation without any interest in sound, and the
// simulation itself does not know a sound system exists. See sim/events.h.
void stepCombat(const Level& level, CombatState& state, Body& ship,
                const glm::quat& shipOrientation, const WeaponConfig& cfg,
                const FireControls& controls, float dt, RobotState* robots,
                const SolidDoors* doors = nullptr, EventQueue* events = nullptr,
                PropState* props = nullptr);

// A short-lived additive billboard with a light: a muzzle flash, an impact, an
// explosion. Public because robots fire too, and a robot's muzzle flash is the
// same thing as the player's.
void spawnEffect(CombatState& state, const glm::vec3& position, int segment,
                 const glm::vec3& color, float size, float lifetime, float lightRadius,
                 float lightIntensity);

// Resets the player after a death. Returns true if a death happened this step.
bool checkPlayerDeath(const Level& level, CombatState& state, Body& ship,
                      glm::quat& shipOrientation, EventQueue* events = nullptr);

} // namespace dvk::sim
