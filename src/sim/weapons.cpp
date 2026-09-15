#include "sim/weapons.h"

#include "core/log.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace dvk::sim {

namespace {

template <class T>
void get(const nlohmann::json& j, const char* key, T& out) {
    if (auto it = j.find(key); it != j.end() && !it->is_null()) out = it->get<T>();
}

void getVec3(const nlohmann::json& j, const char* key, glm::vec3& out) {
    if (auto it = j.find(key); it != j.end() && it->is_array() && it->size() == 3) {
        out = glm::vec3((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
    }
}

void readWeapon(const nlohmann::json& j, WeaponDef& w) {
    get(j, "damage", w.damage);
    get(j, "speed", w.speed);
    get(j, "fireInterval", w.fireInterval);
    get(j, "energyCost", w.energyCost);
    get(j, "ammoCost", w.ammoCost);
    get(j, "radius", w.radius);
    get(j, "lifetime", w.lifetime);
    get(j, "spread", w.spread);
    getVec3(j, "color", w.color);
    get(j, "lightRadius", w.lightRadius);
    get(j, "lightIntensity", w.lightIntensity);
    get(j, "splashRadius", w.splashRadius);
    get(j, "splashDamage", w.splashDamage);
    get(j, "impactImpulse", w.impactImpulse);
    get(j, "splashImpulse", w.splashImpulse);
    get(j, "turnRate", w.turnRate);
    get(j, "seekCone", w.seekCone);
    get(j, "seekRange", w.seekRange);
}

nlohmann::json writeWeapon(const WeaponDef& w) {
    return nlohmann::json{
        {"damage", w.damage},
        {"speed", w.speed},
        {"fireInterval", w.fireInterval},
        {"energyCost", w.energyCost},
        {"ammoCost", w.ammoCost},
        {"radius", w.radius},
        {"lifetime", w.lifetime},
        {"spread", w.spread},
        {"color", {w.color.r, w.color.g, w.color.b}},
        {"lightRadius", w.lightRadius},
        {"lightIntensity", w.lightIntensity},
        {"splashRadius", w.splashRadius},
        {"splashDamage", w.splashDamage},
        {"impactImpulse", w.impactImpulse},
        {"splashImpulse", w.splashImpulse},
        {"turnRate", w.turnRate},
        {"seekCone", w.seekCone},
        {"seekRange", w.seekRange},
    };
}

} // namespace

WeaponConfig WeaponConfig::defaults() {
    WeaponConfig cfg;

    WeaponDef& laser = cfg.weapons[WeaponLaser];
    laser.damage = 8.0f;
    laser.impactImpulse = 5.0f;
    laser.speed = 140.0f;
    laser.fireInterval = 0.18f;
    laser.energyCost = 0.7f;
    laser.radius = 0.35f;
    laser.lifetime = 3.0f;
    laser.color = cfg.laserColor[0];
    laser.lightRadius = 20.0f;
    laser.lightIntensity = 2.6f;

    WeaponDef& vulcan = cfg.weapons[WeaponVulcan];
    vulcan.damage = 3.0f;
    vulcan.impactImpulse = 2.5f;
    vulcan.speed = 220.0f;
    vulcan.fireInterval = 0.05f;     // 20 rounds a second
    vulcan.ammoCost = 1;
    vulcan.radius = 0.2f;
    vulcan.lifetime = 2.0f;
    vulcan.spread = 0.022f;
    vulcan.color = glm::vec3(1.0f, 0.88f, 0.55f);
    vulcan.lightRadius = 11.0f;
    vulcan.lightIntensity = 1.5f;

    WeaponDef& concussion = cfg.weapons[WeaponConcussion];
    concussion.damage = 40.0f;
    concussion.impactImpulse = 14.0f;
    concussion.splashImpulse = 60.0f;
    concussion.speed = 55.0f;
    concussion.fireInterval = 0.65f;
    concussion.ammoCost = 1;
    concussion.radius = 0.6f;
    concussion.lifetime = 6.0f;
    concussion.color = glm::vec3(1.0f, 0.55f, 0.25f);
    concussion.lightRadius = 26.0f;
    concussion.lightIntensity = 3.0f;
    concussion.splashRadius = 12.0f;
    concussion.splashDamage = 28.0f;

    WeaponDef& homing = cfg.weapons[WeaponHoming];
    homing = concussion;
    homing.damage = 34.0f;
    homing.impactImpulse = 11.0f;
    homing.splashImpulse = 48.0f;
    homing.speed = 46.0f;
    homing.fireInterval = 0.8f;
    homing.color = glm::vec3(0.5f, 0.85f, 1.0f);
    homing.splashRadius = 10.0f;
    homing.splashDamage = 22.0f;
    homing.turnRate = 2.6f;
    homing.seekCone = 0.9f;
    homing.seekRange = 140.0f;

    return cfg;
}

WeaponConfig loadWeaponConfig(const std::string& path) {
    WeaponConfig cfg = WeaponConfig::defaults();

    std::ifstream in(path);
    if (!in) {
        log::info("weapon config '{}' not found, using defaults", path);
        return cfg;
    }

    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        log::error("weapon config '{}' is not valid JSON ({}), using defaults", path, e.what());
        return cfg;
    }

    if (auto it = j.find("weapons"); it != j.end()) {
        for (int i = 0; i < WeaponCount; ++i) {
            if (auto w = it->find(kWeaponNames[static_cast<size_t>(i)]); w != it->end()) {
                readWeapon(*w, cfg.weapons[static_cast<size_t>(i)]);
            }
        }
    }
    if (auto it = j.find("laserLevels"); it != j.end()) {
        for (int i = 0; i < kLaserLevels && i < static_cast<int>(it->size()); ++i) {
            get((*it)[static_cast<size_t>(i)], "damage", cfg.laserDamage[static_cast<size_t>(i)]);
            getVec3((*it)[static_cast<size_t>(i)], "color", cfg.laserColor[static_cast<size_t>(i)]);
        }
    }
    get(j, "flareSpeed", cfg.flareSpeed);
    get(j, "flareLifetime", cfg.flareLifetime);
    get(j, "flareFade", cfg.flareFade);
    get(j, "flareRadius", cfg.flareRadius);
    get(j, "flareIntensity", cfg.flareIntensity);
    getVec3(j, "flareColor", cfg.flareColor);
    get(j, "playerMass", cfg.playerMass);
    get(j, "headlightDrain", cfg.headlightDrain);
    get(j, "energyRecharge", cfg.energyRecharge);
    get(j, "shadowBudget", cfg.shadowBudget);

    log::info("weapon config '{}' loaded", path);
    return cfg;
}

bool saveWeaponConfig(const std::string& path, const WeaponConfig& cfg) {
    nlohmann::json j;
    for (int i = 0; i < WeaponCount; ++i) {
        j["weapons"][kWeaponNames[static_cast<size_t>(i)]] =
            writeWeapon(cfg.weapons[static_cast<size_t>(i)]);
    }
    for (int i = 0; i < kLaserLevels; ++i) {
        const glm::vec3& c = cfg.laserColor[static_cast<size_t>(i)];
        j["laserLevels"].push_back(
            {{"damage", cfg.laserDamage[static_cast<size_t>(i)]}, {"color", {c.r, c.g, c.b}}});
    }
    j["flareSpeed"] = cfg.flareSpeed;
    j["flareLifetime"] = cfg.flareLifetime;
    j["flareFade"] = cfg.flareFade;
    j["flareRadius"] = cfg.flareRadius;
    j["flareIntensity"] = cfg.flareIntensity;
    j["flareColor"] = {cfg.flareColor.r, cfg.flareColor.g, cfg.flareColor.b};
    j["playerMass"] = cfg.playerMass;
    j["headlightDrain"] = cfg.headlightDrain;
    j["energyRecharge"] = cfg.energyRecharge;
    j["shadowBudget"] = cfg.shadowBudget;

    std::ofstream out(path);
    if (!out) {
        log::error("cannot write weapon config '{}'", path);
        return false;
    }
    out << j.dump(2) << '\n';
    log::info("weapon config saved to '{}'", path);
    return true;
}

} // namespace dvk::sim
