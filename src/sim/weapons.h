#pragma once

#include <array>
#include <cstdint>
#include <glm/vec3.hpp>
#include <string>

namespace dvk::sim {

enum WeaponId : int { WeaponLaser = 0, WeaponVulcan, WeaponConcussion, WeaponHoming, WeaponCount };

inline constexpr std::array<const char*, WeaponCount> kWeaponNames{"laser", "vulcan",
                                                                  "concussion", "homing"};

// Which trigger a weapon answers to. Written out per weapon rather than
// inferred from the enum's order: "id <= WeaponVulcan" happened to be true
// while the guns were declared guns-first, and would silently reclassify every
// weapon added after it.
enum class WeaponSlot : uint8_t { Primary, Secondary };

constexpr WeaponSlot weaponSlot(int weaponId) {
    switch (static_cast<WeaponId>(weaponId)) {
        case WeaponLaser:
        case WeaponVulcan:
            return WeaponSlot::Primary;
        case WeaponConcussion:
        case WeaponHoming:
            return WeaponSlot::Secondary;
        case WeaponCount:
            break;
    }
    return WeaponSlot::Primary;
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
