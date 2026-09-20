#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <glm/vec3.hpp>
#include <string>

namespace dvk::sim {

enum WeaponId : int {
    WeaponLaser = 0,
    WeaponVulcan,
    WeaponConcussion,
    WeaponHoming,
    WeaponRailgun,
    WeaponLightning,
    WeaponCount,
};

inline constexpr std::array<const char*, WeaponCount> kWeaponNames{
    "laser", "vulcan", "concussion", "homing", "railgun", "lightning"};

// The same weapons, named short enough to fit a cell of the HUD's weapon strip,
// where "concussion" does not. Same order and same length as kWeaponNames,
// because both are indexed by WeaponId and a second order would be one more
// thing to keep in step.
inline constexpr std::array<const char*, WeaponCount> kWeaponShortNames{
    "laser", "vulcan", "conc", "homg", "rail", "ltng"};

// Which trigger a weapon answers to. Written out per weapon rather than
// inferred from the enum's order: "id <= WeaponVulcan" happened to be true
// while the guns were declared guns-first, and would silently reclassify every
// weapon added after it.
enum class WeaponSlot : uint8_t { Primary, Secondary };

constexpr WeaponSlot weaponSlot(int weaponId) {
    switch (static_cast<WeaponId>(weaponId)) {
        case WeaponLaser:
        case WeaponVulcan:
        case WeaponRailgun:
        case WeaponLightning:
            return WeaponSlot::Primary;
        case WeaponConcussion:
        case WeaponHoming:
            return WeaponSlot::Secondary;
        case WeaponCount:
            break;
    }
    return WeaponSlot::Primary;
}

// The order the missiles cycle in. The primaries each have a key of their own
// and so have no order here; which key that is belongs to game/input_actions.h,
// because the simulation knows nothing about keyboards. The cycle does live
// here: it reads the current selection and picks the next, which is state the
// simulation owns.
inline constexpr std::array<WeaponId, 2> kSecondaryWeapons{WeaponConcussion, WeaponHoming};

// The next secondary in the cycle. An id that is not a secondary at all - a
// primary, or a corrupt save - starts the cycle from the beginning rather than
// returning something that is not a missile.
constexpr WeaponId nextSecondaryWeapon(int weaponId) {
    for (size_t i = 0; i < kSecondaryWeapons.size(); ++i) {
        if (kSecondaryWeapons[i] == weaponId) {
            return kSecondaryWeapons[(i + 1) % kSecondaryWeapons.size()];
        }
    }
    return kSecondaryWeapons[0];
}

// How a weapon delivers its damage. Structural rather than tunable, so it is a
// function of the id and not a field anyone can set to a nonsense combination.
//
//   Projectile   spawns a bolt that travels and is swept against the world.
//   Instant      one trace per trigger pull, at fireInterval. Damage and
//                energy are per shot.
//   Continuous   one trace every step the trigger is held. Damage and energy
//                are per SECOND and are multiplied by dt; fireInterval is the
//                gap between its sound cues, not a rate limit on damage.
enum class WeaponFire : uint8_t { Projectile, Instant, Continuous };

constexpr WeaponFire weaponFire(int weaponId) {
    switch (static_cast<WeaponId>(weaponId)) {
        case WeaponLaser:
        case WeaponVulcan:
        case WeaponConcussion:
        case WeaponHoming:
            return WeaponFire::Projectile;
        case WeaponRailgun:
            return WeaponFire::Instant;
        case WeaponLightning:
            return WeaponFire::Continuous;
        case WeaponCount:
            break;
    }
    return WeaponFire::Projectile;
}

// Whether a shot leaves carrying the ship's own velocity. Structural like
// weaponFire, and written out per weapon for the same reason.
//
// The guns inherit. At 140 and 220 units/s a ship doing 20 is a rounding error
// on the bolt, and inheriting is what keeps a burst looking like it left the
// ship rather than being spat out beside it.
//
// The missiles do not. At 55 units/s the ship's own velocity is a third of the
// missile's: it swung the world speed from 35 to 75 and the range with it, and
// on a full strafe it tilted the missile some twenty degrees off the crosshair.
// A weapon that commits you for two thirds of a second and detonates twelve
// units wide has to go where the reticle is and arrive when it always arrives.
constexpr bool weaponInheritsVelocity(int weaponId) {
    switch (static_cast<WeaponId>(weaponId)) {
        case WeaponConcussion:
        case WeaponHoming:
            return false;
        case WeaponLaser:
        case WeaponVulcan:
        case WeaponRailgun:
        case WeaponLightning:
        case WeaponCount:
            break;
    }
    return true;
}

// Laser has four upgrade levels. Each changes damage and colour, and the colour
// is the readable part: in a mine with no ambient light, what your own fire
// looks like is most of what you see.
inline constexpr int kLaserLevels = 4;

struct WeaponDef {
    float damage = 10.0f;
    float speed = 120.0f;          // units/s
    float fireInterval = 0.25f;    // seconds between shots
    float energyCost = 0.0f;
    int ammoCost = 0;              // vulcan and the missiles draw on counts, not energy
    float radius = 0.35f;          // projectile sphere, for the sweep
    float lifetime = 4.0f;         // seconds before it expires on its own
    float spread = 0.0f;           // radians, half-angle of the random cone
    glm::vec3 color{1.0f};
    float lightRadius = 18.0f;
    float lightIntensity = 2.4f;
    float splashRadius = 0.0f;
    float splashDamage = 0.0f;
    // The punch. impactImpulse is what a direct hit shoves the target with;
    // splashImpulse is what the blast shoves everything in its radius with, at
    // the centre, falling off to nothing at the edge. Both are a velocity
    // change in units/s at mass 1 - see sim/body.h.
    float impactImpulse = 0.0f;
    float splashImpulse = 0.0f;
    // Hitscan only: how far the trace reaches, and what its tracer looks like.
    // A beam is drawn from the muzzle to whatever the trace found, so range is
    // visible as well as felt.
    float range = 0.0f;            // units
    float beamWidth = 0.12f;       // units, half the drawn thickness
    float beamTime = 0.10f;        // seconds the tracer lingers
    // Homing only.
    float turnRate = 0.0f;         // rad/s
    float seekCone = 0.0f;         // radians, half-angle it will acquire within
    float seekRange = 0.0f;
};

struct WeaponConfig {
    std::array<WeaponDef, WeaponCount> weapons{};
    // Per laser level, since that is the only weapon that upgrades.
    std::array<float, kLaserLevels> laserDamage{{8.0f, 12.0f, 17.0f, 23.0f}};
    std::array<glm::vec3, kLaserLevels> laserColor{{
        glm::vec3(1.00f, 0.35f, 0.25f),   // red
        glm::vec3(1.00f, 0.70f, 0.20f),   // amber
        glm::vec3(0.45f, 1.00f, 0.40f),   // green
        glm::vec3(0.60f, 0.55f, 1.00f),   // violet
    }};

    // Flares are not a weapon, but they are thrown like one.
    float flareSpeed = 34.0f;
    float flareLifetime = 30.0f;
    float flareFade = 4.0f;          // seconds of fade at the end of the life
    float flareRadius = 26.0f;
    float flareIntensity = 3.0f;
    glm::vec3 flareColor{1.0f, 0.72f, 0.36f};

    // Player resources.
    // How hard the ship is to shove. Heavier than a drone and lighter than a
    // brute: a concussion at your own feet throws you, and a vulcan round does
    // not. Lives here rather than in FlightConfig because this is the config
    // stepCombat is handed, and stepCombat is what does the shoving.
    float playerMass = 1.5f;
    float headlightDrain = 1.6f;     // energy per second while the headlight is on
    float energyRecharge = 0.0f;     // none by default; pickups are M7's job

    // At most this many shadow-casting lights per frame. Every light still
    // lights; the ones over budget simply stop casting. See the note in
    // docs/ARCHITECTURE.md - this is the one number that decides whether a
    // corridor full of vulcan fire stays playable.
    int shadowBudget = 16;

    static WeaponConfig defaults();
};

WeaponConfig loadWeaponConfig(const std::string& path);
bool saveWeaponConfig(const std::string& path, const WeaponConfig& cfg);

} // namespace dvk::sim
