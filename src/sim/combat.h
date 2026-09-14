#pragma once

#include "sim/body.h"
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

inline constexpr int kMaxProjectiles = 128;
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

// The player's resources and loadout.
struct PlayerState {
    float shields = 100.0f;
    float energy = 100.0f;
    int weapon = WeaponLaser;
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
    bool headlightOn = true;
    int deaths = 0;

    // Feel, not state anything reads back. Both decay in stepCombat and both
    // only ever leave the simulation through RenderState and HudData: the
    // screen flinches, the ship does not.
    float hurtTimer = 0.0f;     // seconds since the hull was last hit
    float recoilTimer = 0.0f;   // seconds since a missile shoved the ship
};

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
    int selectWeapon = -1;     // 0..3, or -1 for no change
};

struct CombatState {
    ProjectilePool projectiles;
    FlarePool flares;
    EffectPool effects;
    PlayerState player;
    ObjectHealth objects;
};

struct RobotState;
struct RobotConfig;
struct SolidDoors;

// Sets up object health from the level's objects. An object whose type names
// a robot definition is not scenery: the simulation owns it as a robot, so it
// is marked dead here and never acts as a target. Before this, every robot's
// spawn point was an invisible, indestructible sphere that stopped shots.
void initObjects(const Level& level, const RobotConfig& robots, ObjectHealth& out);

// One fixed step of everything in this file. `ship` is the player's body, used
// for fire points, as a target for robot fire, and for taking splash damage.
// `robots` may be null before M6's robots exist. `doors` is the shut-door set
// for this step; a bolt stops at a shut door as it does at a wall.
void stepCombat(const Level& level, CombatState& state, Body& ship,
                const glm::quat& shipOrientation, const WeaponConfig& cfg,
                const FireControls& controls, float dt, RobotState* robots,
                const SolidDoors* doors = nullptr);

// A short-lived additive billboard with a light: a muzzle flash, an impact, an
// explosion. Public because robots fire too, and a robot's muzzle flash is the
// same thing as the player's.
void spawnEffect(CombatState& state, const glm::vec3& position, int segment,
                 const glm::vec3& color, float size, float lifetime, float lightRadius,
                 float lightIntensity);

// Resets the player after a death. Returns true if a death happened this step.
bool checkPlayerDeath(const Level& level, CombatState& state, Body& ship,
                      glm::quat& shipOrientation);

} // namespace dvk::sim
