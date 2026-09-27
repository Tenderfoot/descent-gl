#include "sim/robots.h"

#include "core/log.h"
#include "game/level.h"
#include "sim/collision.h"
#include "sim/lighting.h"
#include "sim/combat.h"

#include <nlohmann/json.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace dvk::sim {

namespace {

uint32_t g_rng = 0x9E3779B9u;

float randUnit() {
    g_rng = g_rng * 1664525u + 1013904223u;
    return static_cast<float>((g_rng >> 8) & 0xFFFFFFu) / 16777215.0f;
}

float randSigned() { return randUnit() * 2.0f - 1.0f; }

template <class T>
void get(const nlohmann::json& j, const char* key, T& out) {
    if (auto it = j.find(key); it != j.end() && !it->is_null()) out = it->get<T>();
}

void getVec3(const nlohmann::json& j, const char* key, glm::vec3& out) {
    if (auto it = j.find(key); it != j.end() && it->is_array() && it->size() == 3) {
        out = glm::vec3((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
    }
}

nlohmann::json vec3Json(const glm::vec3& v) { return {v.x, v.y, v.z}; }

// A player weapon by its weapons.json key, or `fallback` for a name that is not
// one - with a warning, since a typo here makes an attachment unbreakable.
int weaponByName(const std::string& name, int fallback) {
    for (size_t i = 0; i < kWeaponNames.size(); ++i) {
        if (name == kWeaponNames[i]) return static_cast<int>(i);
    }
    log::warn("robot config: '{}' is not a weapon; keeping '{}'", name,
              kWeaponNames[static_cast<size_t>(fallback)]);
    return fallback;
}

// A pickup kind by its level-file name, or nothing for a name that is not one.
bool pickupByName(const std::string& name, PickupKind& out) {
    for (size_t i = 0; i < kPickupNames.size(); ++i) {
        if (name == kPickupNames[i]) {
            out = static_cast<PickupKind>(i);
            return true;
        }
    }
    log::warn("robot config: '{}' is not a pickup; not dropping it", name);
    return false;
}

void readAttachment(const nlohmann::json& j, AttachmentDef& a) {
    get(j, "model", a.model);
    getVec3(j, "mount", a.mount);
    if (auto it = j.find("breaksOn"); it != j.end() && it->is_string()) {
        a.breaksOn = weaponByName(it->get<std::string>(), a.breaksOn);
    }
    get(j, "breakDamage", a.breakDamage);
}

nlohmann::json writeAttachment(const AttachmentDef& a) {
    nlohmann::json j;
    j["model"] = a.model;
    j["mount"] = vec3Json(a.mount);
    j["breaksOn"] = kWeaponNames[static_cast<size_t>(a.breaksOn)];
    j["breakDamage"] = a.breakDamage;
    return j;
}

void readSpecial(const nlohmann::json& j, SpecialAttackDef& sp) {
    get(j, "enabled", sp.enabled);
    if (auto it = j.find("delivery"); it != j.end() && it->is_string()) {
        const std::string name = it->get<std::string>();
        bool known = false;
        for (size_t i = 0; i < kSpecialDeliveryNames.size(); ++i) {
            if (name == kSpecialDeliveryNames[i]) {
                sp.delivery = static_cast<SpecialDelivery>(i);
                known = true;
            }
        }
        if (!known) log::warn("robot config: '{}' is not a special attack delivery", name);
    }
    getVec3(j, "muzzle", sp.muzzle);
    get(j, "windup", sp.windup);
    get(j, "cooldown", sp.cooldown);
    get(j, "damage", sp.damage);
    get(j, "impulse", sp.impulse);
    getVec3(j, "color", sp.color);
    get(j, "orbSize", sp.orbSize);
    get(j, "lightRadius", sp.lightRadius);
    get(j, "lightIntensity", sp.lightIntensity);
    get(j, "pulseStart", sp.pulseStart);
    get(j, "pulseEnd", sp.pulseEnd);
    get(j, "range", sp.range);
    get(j, "beamWidth", sp.beamWidth);
    get(j, "beamTime", sp.beamTime);
    get(j, "projectileSpeed", sp.projectileSpeed);
    get(j, "projectileRadius", sp.projectileRadius);
    get(j, "projectileLifetime", sp.projectileLifetime);
    get(j, "splashRadius", sp.splashRadius);
    get(j, "splashDamage", sp.splashDamage);
    get(j, "splashImpulse", sp.splashImpulse);
    get(j, "homing", sp.homing);
    get(j, "turnRate", sp.turnRate);
    get(j, "shootable", sp.shootable);
}

nlohmann::json writeSpecial(const SpecialAttackDef& sp) {
    nlohmann::json j;
    j["enabled"] = sp.enabled;
    j["delivery"] = kSpecialDeliveryNames[static_cast<size_t>(sp.delivery)];
    j["muzzle"] = vec3Json(sp.muzzle);
    j["windup"] = sp.windup;
    j["cooldown"] = sp.cooldown;
    j["damage"] = sp.damage;
    j["impulse"] = sp.impulse;
    j["color"] = vec3Json(sp.color);
    j["orbSize"] = sp.orbSize;
    j["lightRadius"] = sp.lightRadius;
    j["lightIntensity"] = sp.lightIntensity;
    j["pulseStart"] = sp.pulseStart;
    j["pulseEnd"] = sp.pulseEnd;
    // Only the delivery's own numbers, for the same reason the whole block
    // is only written for a type that has one.
    if (sp.delivery == SpecialDelivery::Hitscan) {
        j["range"] = sp.range;
        j["beamWidth"] = sp.beamWidth;
        j["beamTime"] = sp.beamTime;
    } else {
        j["projectileSpeed"] = sp.projectileSpeed;
        j["projectileRadius"] = sp.projectileRadius;
        j["projectileLifetime"] = sp.projectileLifetime;
        j["splashRadius"] = sp.splashRadius;
        j["splashDamage"] = sp.splashDamage;
        j["splashImpulse"] = sp.splashImpulse;
        j["homing"] = sp.homing;
        j["turnRate"] = sp.turnRate;
        j["shootable"] = sp.shootable;
    }
    return j;
}

void setMode(Robot& robot, RobotMode mode) {
    if (robot.mode == mode) return;
    robot.previousMode = robot.mode;
    robot.mode = mode;
    robot.modeTime = 0.0f;
}

// Turns the robot towards `direction` at no more than turnRate.
void faceTowards(Robot& robot, const glm::vec3& direction, float turnRate, float dt) {
    if (glm::length(direction) < 1e-4f) return;
    const glm::quat want =
        glm::quatLookAt(glm::normalize(direction), glm::vec3(0.0f, 1.0f, 0.0f));
    const float maxStep = turnRate * dt;
    const float dot = std::clamp(glm::dot(robot.orientation, want), -1.0f, 1.0f);
    const float angle = 2.0f * std::acos(std::abs(dot));
    const float t = angle > maxStep && angle > 1e-5f ? maxStep / angle : 1.0f;
    robot.orientation = glm::normalize(glm::slerp(robot.orientation, want, t));
}

void steer(Robot& robot, const glm::vec3& desired, const RobotDef& def, float dt) {
    const glm::vec3 target = desired * def.speed;
    const glm::vec3 delta = target - robot.body.velocity;
    const float maxChange = def.acceleration * dt;
    const float length = glm::length(delta);
    robot.body.velocity += length > maxChange ? delta * (maxChange / length) : delta;
}

// Puts a robot's attachment where the robot now is: its origin, its muzzle
// and its hitbox, all from the one transform the renderer also uses.
void placeAttachment(Robot& robot, const RobotDef& def) {
    robot.attachmentOrigin = robot.body.position + robot.orientation * def.attachment.mount;
    robot.muzzle = robot.attachmentOrigin + robot.orientation * def.special.muzzle;
    robot.attachmentProxy =
        placeProxy(def.attachment.volume, robot.attachmentOrigin, robot.orientation, 1.0f);
}

} // namespace

RobotConfig RobotConfig::defaults() {
    RobotConfig cfg;

    RobotDef drone;
    drone.name = "drone";
    drone.model = "drone";
    drone.health = 26.0f;
    drone.radius = 1.9f;
    drone.mass = 0.8f;
    drone.speed = 19.0f;
    drone.acceleration = 34.0f;
    drone.turnRate = 3.0f;
    drone.attackRange = 45.0f;
    drone.aggression = 0.8f;
    drone.retreatThreshold = 0.35f;
    drone.projectileSpeed = 80.0f;
    drone.projectileDamage = 4.0f;
    drone.projectileImpulse = 4.0f;
    drone.fireInterval = 0.3f;
    drone.burstCount = 2;
    drone.burstPause = 1.2f;
    drone.spread = 0.07f;
    drone.lead = 0.5f;
    drone.color = glm::vec3(0.85f, 0.4f, 0.35f);
    drone.eyeColor = glm::vec3(1.0f, 0.3f, 0.2f);
    drone.eyeSize = 0.3f;
    drone.eyes = {glm::vec3(-0.55f, 0.38f, -1.64f), glm::vec3(0.55f, 0.38f, -1.64f)};
    cfg.types.push_back(drone);

    RobotDef gunner;
    gunner.name = "gunner";
    gunner.model = "gunner";
    gunner.health = 48.0f;
    gunner.radius = 2.2f;
    gunner.mass = 1.2f;
    gunner.speed = 13.0f;
    gunner.acceleration = 22.0f;
    gunner.turnRate = 2.0f;
    gunner.attackRange = 75.0f;
    gunner.aggression = 0.55f;
    gunner.retreatThreshold = 0.3f;
    gunner.projectileSpeed = 105.0f;
    gunner.projectileDamage = 8.0f;
    gunner.projectileImpulse = 6.0f;
    gunner.fireInterval = 0.22f;
    gunner.burstCount = 4;
    gunner.burstPause = 1.6f;
    gunner.spread = 0.045f;
    gunner.lead = 0.75f;
    gunner.color = glm::vec3(0.5f, 0.75f, 0.95f);
    gunner.eyeColor = glm::vec3(0.5f, 0.9f, 1.0f);
    gunner.eyeSize = 0.34f;
    gunner.eyes = {glm::vec3(-0.765f, 0.615f, -1.44f), glm::vec3(0.765f, 0.615f, -1.44f)};
    gunner.chargeDrops = {PickupKind::VulcanAmmo};
    cfg.types.push_back(gunner);

    RobotDef brute;
    brute.name = "brute";
    brute.model = "brute";
    brute.health = 95.0f;
    brute.radius = 3.1f;
    brute.mass = 2.8f;
    brute.speed = 8.5f;
    brute.acceleration = 14.0f;
    brute.turnRate = 1.3f;
    brute.attackRange = 55.0f;
    brute.aggression = 0.95f;   // barely retreats; it is the wall you run into
    brute.retreatThreshold = 0.15f;
    brute.projectileSpeed = 70.0f;
    brute.projectileDamage = 14.0f;
    brute.projectileImpulse = 9.0f;
    brute.fireInterval = 0.5f;
    brute.burstCount = 2;
    brute.burstPause = 2.0f;
    brute.spread = 0.09f;
    brute.lead = 0.55f;
    brute.color = glm::vec3(0.95f, 0.7f, 0.3f);
    brute.eyeColor = glm::vec3(1.0f, 0.75f, 0.25f);
    brute.eyeSize = 0.46f;
    brute.eyes = {glm::vec3(-0.96f, 1.815f, -1.82f), glm::vec3(0.96f, 1.815f, -1.82f),
                  glm::vec3(0.0f, 2.015f, -1.99f)};
    brute.chargeDrops = {PickupKind::Concussion};
    cfg.types.push_back(brute);

    // The same brute with a railgun over its right shoulder. A type of its own
    // rather than a flag on a level object, so a level asks for one by name
    // and the level format does not change. Its `model` is the plain brute:
    // the gun is drawn as a separate mesh because it can come off.
    RobotDef railBrute = brute;
    railBrute.name = "brute_railgun";
    railBrute.attachment.model = "railgun";
    railBrute.attachment.mount = glm::vec3(2.2f, 2.0f, -1.1f);
    railBrute.attachment.breaksOn = WeaponRailgun;
    railBrute.special.enabled = true;
    railBrute.special.delivery = SpecialDelivery::Hitscan;
    railBrute.special.muzzle = glm::vec3(0.0f, 0.0f, -2.75f);
    railBrute.special.windup = 5.0f;
    railBrute.special.cooldown = 4.0f;
    railBrute.special.damage = 32.0f;
    railBrute.special.impulse = 30.0f;
    railBrute.special.color = glm::vec3(0.55f, 0.8f, 1.0f);
    railBrute.special.orbSize = 0.9f;
    railBrute.special.lightRadius = 24.0f;
    railBrute.special.lightIntensity = 4.5f;
    railBrute.special.pulseStart = 0.55f;
    railBrute.special.pulseEnd = 0.12f;
    railBrute.special.range = 400.0f;
    railBrute.special.beamWidth = 0.3f;
    railBrute.special.beamTime = 0.35f;
    railBrute.chargeDrops = {PickupKind::Concussion, PickupKind::RailgunAmmo};
    cfg.types.push_back(railBrute);

    // The gunner with a missile pod on its right shoulder: after a three
    // second wind-up, one homing missile. Slower than the ship's own missiles
    // and turning well under what the ship can, so it can be outflown; and
    // shootable, so it can be shot down - where its blast hurts the gunner
    // that fired it as readily as anything else. The pod comes off to the
    // railgun, like the brute's gun: one rule for the player to learn.
    RobotDef missileGunner = gunner;
    missileGunner.chargeDrops = {PickupKind::Homing, PickupKind::VulcanAmmo};
    missileGunner.name = "gunner_missile";
    missileGunner.attachment.model = "missile_pod";
    missileGunner.attachment.mount = glm::vec3(1.3f, 2.05f, 0.0f);
    missileGunner.attachment.breaksOn = WeaponRailgun;
    missileGunner.special.enabled = true;
    missileGunner.special.delivery = SpecialDelivery::Projectile;
    missileGunner.special.muzzle = glm::vec3(0.0f, 0.0f, -1.4f);
    missileGunner.special.windup = 3.0f;
    missileGunner.special.cooldown = 5.0f;
    missileGunner.special.damage = 18.0f;
    missileGunner.special.impulse = 12.0f;
    missileGunner.special.color = glm::vec3(1.0f, 0.45f, 0.2f);
    missileGunner.special.orbSize = 0.6f;
    missileGunner.special.lightRadius = 18.0f;
    missileGunner.special.lightIntensity = 3.5f;
    missileGunner.special.pulseStart = 0.45f;
    missileGunner.special.pulseEnd = 0.15f;
    missileGunner.special.projectileSpeed = 36.0f;
    missileGunner.special.projectileRadius = 0.55f;
    missileGunner.special.projectileLifetime = 7.0f;
    missileGunner.special.splashRadius = 9.0f;
    missileGunner.special.splashDamage = 14.0f;
    missileGunner.special.splashImpulse = 22.0f;
    missileGunner.special.homing = true;
    missileGunner.special.turnRate = 1.4f;
    missileGunner.special.shootable = true;
    cfg.types.push_back(missileGunner);

    RobotDef reactor;
    reactor.name = "reactor";
    reactor.model = "reactor";
    reactor.isReactor = true;
    reactor.health = 600.0f;
    reactor.radius = 5.5f;
    reactor.mass = 600.0f;
    reactor.speed = 0.0f;
    reactor.acceleration = 0.0f;
    reactor.turnRate = 0.8f;
    reactor.attackRange = 130.0f;
    reactor.aggression = 1.0f;      // never retreats; it cannot move anyway
    reactor.retreatThreshold = -1.0f;
    reactor.projectileSpeed = 48.0f;
    reactor.projectileDamage = 16.0f;
    reactor.projectileImpulse = 7.0f;
    reactor.fireInterval = 0.8f;
    reactor.burstCount = 2;
    reactor.burstPause = 1.8f;
    reactor.spread = 0.06f;
    reactor.lead = 0.5f;
    reactor.color = glm::vec3(1.0f, 0.55f, 0.2f);
    reactor.eyeColor = glm::vec3(1.0f, 0.62f, 0.22f);
    reactor.eyeSize = 2.6f;
    reactor.eyes = {glm::vec3(0.0f, -0.04f, -2.9f)};
    reactor.dropNothing = 1.0f;
    reactor.dropEnergy = 0.0f;
    reactor.dropShield = 0.0f;
    cfg.types.push_back(reactor);

    return cfg;
}

RobotConfig loadRobotConfig(const std::string& path) {
    RobotConfig cfg = RobotConfig::defaults();

    std::ifstream in(path);
    if (!in) {
        log::info("robot config '{}' not found, using defaults", path);
        return cfg;
    }
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        log::error("robot config '{}' is not valid JSON ({}), using defaults", path, e.what());
        return cfg;
    }

    if (auto it = j.find("types"); it != j.end() && it->is_array()) {
        for (const auto& tj : *it) {
            const std::string name = tj.value("name", std::string());
            if (name.empty()) continue;
            const int index = cfg.indexOf(name);
            RobotDef& def = index >= 0 ? cfg.types[static_cast<size_t>(index)]
                                       : cfg.types.emplace_back();
            def.name = name;
            get(tj, "model", def.model);
            if (def.model.empty()) def.model = name;
            get(tj, "health", def.health);
            get(tj, "energy", def.energy);
            get(tj, "radius", def.radius);
            get(tj, "speed", def.speed);
            get(tj, "acceleration", def.acceleration);
            get(tj, "turnRate", def.turnRate);
            get(tj, "attackRange", def.attackRange);
            get(tj, "aggression", def.aggression);
            get(tj, "retreatThreshold", def.retreatThreshold);
            get(tj, "retreatTime", def.retreatTime);
            get(tj, "projectileSpeed", def.projectileSpeed);
            get(tj, "projectileDamage", def.projectileDamage);
            get(tj, "fireInterval", def.fireInterval);
            get(tj, "burstCount", def.burstCount);
            get(tj, "burstPause", def.burstPause);
            get(tj, "spread", def.spread);
            get(tj, "lead", def.lead);
            getVec3(tj, "color", def.color);
            getVec3(tj, "eyeColor", def.eyeColor);
            get(tj, "mass", def.mass);
            get(tj, "projectileImpulse", def.projectileImpulse);
            get(tj, "eyeSize", def.eyeSize);
            get(tj, "isReactor", def.isReactor);
            get(tj, "dropNothing", def.dropNothing);
            get(tj, "dropEnergy", def.dropEnergy);
            get(tj, "dropShield", def.dropShield);
            if (auto cd = tj.find("chargeDrops"); cd != tj.end() && cd->is_array()) {
                def.chargeDrops.clear();
                for (const auto& name : *cd) {
                    PickupKind kind = PickupKind::Energy;
                    if (name.is_string() && pickupByName(name.get<std::string>(), kind)) {
                        def.chargeDrops.push_back(kind);
                    }
                }
            }
            if (auto a = tj.find("attachment"); a != tj.end() && a->is_object()) {
                readAttachment(*a, def.attachment);
            }
            if (auto sp = tj.find("special"); sp != tj.end() && sp->is_object()) {
                readSpecial(*sp, def.special);
            }
            if (auto e = tj.find("eyes"); e != tj.end() && e->is_array()) {
                def.eyes.clear();
                for (const auto& ej : *e) {
                    if (ej.is_array() && ej.size() == 3) {
                        def.eyes.emplace_back(ej[0].get<float>(), ej[1].get<float>(),
                                              ej[2].get<float>());
                    }
                }
            }
        }
    }

    log::info("robot config '{}' loaded: {} types", path, cfg.types.size());
    return cfg;
}

bool saveRobotConfig(const std::string& path, const RobotConfig& cfg) {
    nlohmann::json j;
    for (const RobotDef& def : cfg.types) {
        nlohmann::json tj;
        tj["name"] = def.name;
        tj["model"] = def.model;
        tj["health"] = def.health;
        tj["energy"] = def.energy;
        tj["radius"] = def.radius;
        tj["speed"] = def.speed;
        tj["acceleration"] = def.acceleration;
        tj["turnRate"] = def.turnRate;
        tj["attackRange"] = def.attackRange;
        tj["aggression"] = def.aggression;
        tj["retreatThreshold"] = def.retreatThreshold;
        tj["retreatTime"] = def.retreatTime;
        tj["projectileSpeed"] = def.projectileSpeed;
        tj["projectileDamage"] = def.projectileDamage;
        tj["fireInterval"] = def.fireInterval;
        tj["burstCount"] = def.burstCount;
        tj["burstPause"] = def.burstPause;
        tj["spread"] = def.spread;
        tj["lead"] = def.lead;
        tj["color"] = {def.color.r, def.color.g, def.color.b};
        tj["eyeColor"] = {def.eyeColor.r, def.eyeColor.g, def.eyeColor.b};
        tj["mass"] = def.mass;
        tj["projectileImpulse"] = def.projectileImpulse;
        tj["eyeSize"] = def.eyeSize;
        tj["isReactor"] = def.isReactor;
        tj["dropNothing"] = def.dropNothing;
        tj["dropEnergy"] = def.dropEnergy;
        tj["dropShield"] = def.dropShield;
        if (!def.chargeDrops.empty()) {
            nlohmann::json names = nlohmann::json::array();
            for (PickupKind kind : def.chargeDrops) {
                names.push_back(kPickupNames[static_cast<size_t>(kind)]);
            }
            tj["chargeDrops"] = std::move(names);
        }
        // Only for a type that carries them, so the other types' entries do
        // not fill up with the tuning of things they do not have.
        if (def.attachment.present()) tj["attachment"] = writeAttachment(def.attachment);
        if (def.special.enabled) tj["special"] = writeSpecial(def.special);
        for (const glm::vec3& e : def.eyes) tj["eyes"].push_back({e.x, e.y, e.z});
        j["types"].push_back(tj);
    }

    std::ofstream out(path);
    if (!out) {
        log::error("cannot write robot config '{}'", path);
        return false;
    }
    out << j.dump(2) << '\n';
    log::info("robot config saved to '{}'", path);
    return true;
}

void knockAttachmentLoose(const Level& level, RobotState& state, int slot, const glm::vec3& push,
                          const SolidDoors* doors) {
    if (slot < 0 || slot >= state.count) return;
    Robot& robot = state.robots[static_cast<size_t>(slot)];
    if (!robot.attachmentOn) return;
    robot.attachmentOn = false;
    robot.windingUp = false;
    robot.windupTime = 0.0f;

    LooseAttachment& loose = state.looseAttachments[static_cast<size_t>(slot)];
    loose = LooseAttachment{};
    loose.alive = true;
    loose.type = robot.type;
    loose.body.radius = 0.9f;
    loose.body.mass = 0.6f;
    // An attachment can reach past the robot's collision sphere, so against a
    // wall its origin can be in the rock. Walked out from the robot's centre
    // instead, stopping short of anything in the way, so it starts inside the
    // mine.
    loose.body.position = robot.attachmentOrigin;
    loose.body.segment = robot.body.segment;
    const glm::vec3 out = robot.attachmentOrigin - robot.body.position;
    const float reach = glm::length(out);
    if (reach > 1e-4f) {
        const RayHit blocked = raycast(level, robot.body.position, out / reach,
                                       reach + loose.body.radius, robot.body.segment, doors);
        const float clear = blocked.hit ? std::max(0.0f, blocked.distance - loose.body.radius)
                                        : reach;
        loose.body.position = robot.body.position + out * (std::min(clear, reach) / reach);
        loose.body.segment = trackSegment(level, loose.body.position, robot.body.segment);
    }
    // Away from the robot as well as the way it was hit, so even a shot
    // straight down its length throws it clear rather than into the hull.
    const glm::vec3 away = reach > 1e-4f ? out / reach
                                         : robot.orientation * glm::vec3(0.0f, 1.0f, 0.0f);
    loose.body.velocity =
        robot.body.motion() + away * 7.0f + push / loose.body.mass * 0.25f;
    loose.orientation = robot.orientation;
    loose.spin = glm::vec3(randSigned() * 2.5f, randSigned() * 1.5f, 3.0f + randUnit() * 3.0f);
}

bool objectIsRobot(const RobotConfig& cfg, const std::string& type) {
    return cfg.find(type) != nullptr;
}

void spawnRobots(const Level& level, const RobotConfig& cfg, RobotState& out) {
    out = RobotState{};
    for (const LevelObject& obj : level.objects) {
        const int type = cfg.indexOf(obj.type);
        if (type < 0) continue;
        if (out.count >= kMaxRobots) {
            log::warn("more than {} robots in the level; the rest are ignored", kMaxRobots);
            break;
        }
        const RobotDef& def = cfg.types[static_cast<size_t>(type)];

        Robot& robot = out.robots[static_cast<size_t>(out.count++)];
        robot = Robot{};
        robot.alive = true;
        robot.type = type;
        robot.body.position = obj.position;
        robot.body.segment = obj.segment;
        robot.body.radius = def.radius;
        robot.body.mass = def.mass;
        robot.orientation = obj.orientation;
        robot.health = obj.health > 0.0f ? obj.health : def.health;
        robot.energy = def.energy;
        robot.mass = def.mass;
        robot.homeSegment = obj.segment;
        robot.attachmentOn = def.attachment.present();
        robot.attachmentBreaksOn = def.attachment.breaksOn;
        robot.attachmentBreakDamage = def.attachment.breakDamage * def.health;
        if (robot.attachmentOn) placeAttachment(robot, def);
    }
    log::info("{} robots spawned", out.count);
}

namespace {

// A portal a robot cannot get through: a grate, or a shut door it has no way
// to open. Robots carry no keys, so a keyed door is a wall until the player
// opens it; an unkeyed door opens for anything that comes near, so it is a
// corridor.
bool portalLockedShut(const Level& level, const SolidDoors* doors, int segment,
                      const SideData& sd) {
    if (sd.grate >= 0) return true;
    if (sd.door < 0 || !doorShut(doors, segment, sd.neighbour)) return false;
    return level.doors[static_cast<size_t>(sd.door)].key != KeyColor::None;
}

// Breadth-first over the segment graph from the player's segment. One search
// per player-segment change serves every robot: each simply walks downhill.
void rebuildPathField(const Level& level, RobotState& state, int playerSegment,
                      const SolidDoors* doors) {
    const int n = std::min(level.segmentCount(), kMaxPathSegments);
    state.pathValidCount = n;
    for (int i = 0; i < n; ++i) state.distanceToPlayer[static_cast<size_t>(i)] = -1;
    state.pathSegment = playerSegment;
    state.pathDoorCount = doors ? doors->shutCount() : 0;
    if (playerSegment < 0 || playerSegment >= n) return;

    // The queue is the distance array itself walked in order, so no allocation.
    std::array<int16_t, kMaxPathSegments> queue{};
    int head = 0, tail = 0;
    queue[static_cast<size_t>(tail++)] = static_cast<int16_t>(playerSegment);
    state.distanceToPlayer[static_cast<size_t>(playerSegment)] = 0;

    while (head < tail) {
        const int si = queue[static_cast<size_t>(head++)];
        const int16_t d = state.distanceToPlayer[static_cast<size_t>(si)];
        for (const SideData& sd : level.segments[static_cast<size_t>(si)].sides) {
            if (!sd.isPortal() || sd.neighbour >= n) continue;
            if (state.distanceToPlayer[static_cast<size_t>(sd.neighbour)] >= 0) continue;
            // The search runs from the player outwards, so the door is
            // crossed from the neighbour's side; a door is shut from both.
            if (portalLockedShut(level, doors, si, sd)) continue;
            state.distanceToPlayer[static_cast<size_t>(sd.neighbour)] = static_cast<int16_t>(d + 1);
            queue[static_cast<size_t>(tail++)] = static_cast<int16_t>(sd.neighbour);
        }
    }
}

// Where to head to get one segment closer to the player: the centre of the
// portal into the neighbour with the lower distance.
bool nextWaypoint(const Level& level, const RobotState& state, int fromSegment,
                  const SolidDoors* doors, glm::vec3* waypoint) {
    if (fromSegment < 0 || fromSegment >= state.pathValidCount) return false;
    const int16_t here = state.distanceToPlayer[static_cast<size_t>(fromSegment)];
    if (here <= 0) return false;

    const Segment& seg = level.segments[static_cast<size_t>(fromSegment)];
    for (int side = 0; side < SideCount; ++side) {
        const SideData& sd = seg.sides[static_cast<size_t>(side)];
        if (!sd.isPortal() || sd.neighbour >= state.pathValidCount) continue;
        if (portalLockedShut(level, doors, fromSegment, sd)) continue;
        if (state.distanceToPlayer[static_cast<size_t>(sd.neighbour)] == here - 1) {
            *waypoint = sd.center;
            return true;
        }
    }
    return false;
}

// How far from where a robot died its charge-finish ammunition lands, so the
// pickups read as separate things rather than one pile. Nothing checks the
// spot against the mine; a pickup nudged into a wall is an accepted loss.
constexpr float kDropScatter = 2.0f;

void placeDrop(RobotState& state, PickupKind kind, const glm::vec3& position, int segment) {
    for (Drop& drop : state.drops) {
        if (drop.alive) continue;
        drop = Drop{};
        drop.alive = true;
        drop.kind = kind;
        drop.position = position;
        drop.segment = segment;
        return;
    }
}

// A robot finished by the Static Charge always drops energy where it died,
// plus one of each pickup its type lists in `chargeDrops`, spread evenly around
// it from a random start - in place of the type's own roll, including a type
// configured to drop nothing. The reactor is exempt: it is not a kill the loop
// is about.
void spawnDrop(const Level& level, RobotState& state, const RobotDef& def, const Robot& robot) {
    (void)level;
    const glm::vec3& at = robot.body.position;
    const int segment = robot.body.segment;

    if (!def.isReactor && robot.lastHitByCharge) {
        placeDrop(state, PickupKind::Energy, at, segment);
        const size_t count = def.chargeDrops.size();
        const float start = randUnit() * 6.2831853f;
        for (size_t i = 0; i < count; ++i) {
            const float a = start + 6.2831853f * static_cast<float>(i) / static_cast<float>(count);
            const glm::vec3 out(std::cos(a) * kDropScatter, 0.0f, std::sin(a) * kDropScatter);
            placeDrop(state, def.chargeDrops[i], at + out, segment);
        }
        return;
    }

    const float roll = randUnit() * (def.dropNothing + def.dropEnergy + def.dropShield);
    if (roll < def.dropNothing) return;
    placeDrop(state,
              roll < def.dropNothing + def.dropEnergy ? PickupKind::Energy : PickupKind::Shield,
              at, segment);
}

void spawnDebris(RobotState& state, const Robot& robot, const RobotDef& def) {
    const int chunks = 3 + static_cast<int>(randUnit() * 2.0f);
    for (int i = 0; i < chunks; ++i) {
        for (Debris& d : state.debris) {
            if (d.alive) continue;
            d = Debris{};
            d.alive = true;
            d.body.position = robot.body.position;
            d.body.segment = robot.body.segment;
            d.body.radius = 0.4f;
            d.body.mass = 0.2f;
            d.body.velocity =
                glm::vec3(randSigned(), randSigned(), randSigned()) * 18.0f + robot.body.velocity;
            d.color = def.color;
            d.lifetime = 1.8f + randUnit() * 1.4f;
            break;
        }
    }
}

// The end of a special attack's wind-up: one shot from the attachment's
// muzzle, delivered however the type says.
//
// Not dodgeable by design - see SpecialAttackDef. It aims at where the player
// IS when it can see them, with no lead and no spread; when it cannot, it puts
// the shot where it last saw them, which is how a wind-up the player ducked out
// of still lands on the corner they ducked behind.
void fireSpecial(const Level& level, const RobotDef& def, Robot& robot, int slot,
                 CombatState& combat, Body& player, const WeaponConfig& weapons, bool canSee,
                 const SolidDoors* doors, EventQueue* events, const PropState* props,
                 const RobotState& robots) {
    const SpecialAttackDef& sp = def.special;
    robot.windingUp = false;
    robot.windupTime = 0.0f;
    robot.specialCooldown = sp.cooldown;

    const glm::vec3 facing = robot.orientation * glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 target = canSee               ? player.position
                             : robot.hasLastKnown ? robot.lastKnown
                                                  : robot.muzzle + facing * 100.0f;

    // From the muzzle, unless the muzzle is in the rock: an attachment can
    // reach past the robot's collision sphere, and a shot started inside a
    // wall would either stop on it or walk the wrong segment. Then it leaves
    // from the robot's centre instead, which the mine always contains.
    glm::vec3 origin = robot.body.position;
    int segment = robot.body.segment;
    const glm::vec3 out = robot.muzzle - robot.body.position;
    const float reach = glm::length(out);
    if (reach > 1e-4f) {
        const RayHit blocked =
            raycast(level, robot.body.position, out / reach, reach, robot.body.segment, doors);
        if (!blocked.hit) {
            origin = robot.muzzle;
            segment = trackSegment(level, origin, robot.body.segment);
        }
    }

    glm::vec3 dir = target - origin;
    const float length = glm::length(dir);
    dir = length > 1e-4f ? dir / length : facing;

    // The wind-up letting go, at the muzzle, as bright as it got.
    spawnEffect(combat, origin, segment, sp.color, sp.orbSize * 3.0f, 0.2f,
                sp.lightRadius * 1.3f, sp.lightIntensity * 1.5f);

    switch (sp.delivery) {
        case SpecialDelivery::Projectile: {
            // Robot fire like any other bolt from here on: swept, lit, and
            // landing through the same code as the ordinary bursts.
            const int bolt = combat.projectiles.spawn();
            if (bolt < 0) return;
            Projectile& p = combat.projectiles.items[static_cast<size_t>(bolt)];
            p.kind = sp.homing ? ProjectileKind::Homing : ProjectileKind::Laser;
            p.turnRate = sp.turnRate;
            p.targetPlayer = sp.homing;
            p.shootable = sp.shootable;
            p.position = origin;
            p.velocity = dir * sp.projectileSpeed;
            p.color = sp.color;
            p.segment = segment;
            p.radius = sp.projectileRadius;
            p.damage = sp.damage;
            p.impactImpulse = sp.impulse;
            p.splashRadius = sp.splashRadius;
            p.splashDamage = sp.splashDamage;
            p.splashImpulse = sp.splashImpulse;
            p.lifetime = sp.projectileLifetime;
            p.lightRadius = sp.lightRadius * 0.6f;
            p.lightIntensity = sp.lightIntensity * 0.5f;
            p.owner = slot;
            emit(events, sp.homing ? Cue::HomingFire : Cue::RobotFire, origin, segment);
            return;
        }
        case SpecialDelivery::Hitscan:
            break;
    }

    CombatTraceParams params;   // a ray: it has no width in flight
    params.owner = slot;
    const CombatHit hit = traceCombatRay(level, combat, player, &robots, origin, dir, sp.range,
                                         segment, params, doors, props);
    const glm::vec3 end = hit.kind == CombatHit::Kind::None ? origin + dir * sp.range : hit.point;

    const int beamSlot = combat.beams.spawn();
    if (beamSlot >= 0) {
        Beam& beam = combat.beams.items[static_cast<size_t>(beamSlot)];
        beam.start = origin;
        beam.end = end;
        beam.color = sp.color;
        beam.segment = segment;
        beam.width = sp.beamWidth;
        beam.lifetime = sp.beamTime;
    }
    emit(events, Cue::RailgunFire, origin, segment);

    switch (hit.kind) {
        case CombatHit::Kind::Player:
            combat.player.shields -= sp.damage;
            combat.player.hurtTimer = kHurtFlashTime;
            applyImpulse(player, dir, sp.impulse, weapons.playerMass);
            spawnEffect(combat, hit.point, player.segment, sp.color, 2.2f, 0.25f, 14.0f, 3.5f);
            emitLocal(events, Cue::PlayerHurt);
            break;
        case CombatHit::Kind::Object:
            combat.objects.health[static_cast<size_t>(hit.index)] -= sp.damage;
            spawnEffect(combat, hit.point, hit.segment, sp.color, 1.6f, 0.2f, 10.0f, 2.6f);
            emit(events, Cue::WallImpact, hit.point, hit.segment);
            break;
        case CombatHit::Kind::Wall:
        case CombatHit::Kind::Prop:
            spawnEffect(combat, end - dir * 0.2f, hit.segment, sp.color, 1.3f, 0.18f,
                        sp.lightRadius * 0.5f, sp.lightIntensity * 0.55f);
            emit(events, Cue::WallImpact, end, hit.segment);
            break;
        case CombatHit::Kind::Robot:
        case CombatHit::Kind::Missile:
        case CombatHit::Kind::None:
            break;
    }
}

} // namespace

void stepRobots(const Level& level, const RobotConfig& cfg, const WeaponConfig& weapons,
                RobotState& state, CombatState& combat, Body& player, float dt,
                const SolidDoors* doors, EventQueue* events, const PropState* props,
                const OccluderSet* occluders) {
    (void)weapons;

    // Doors a robot still cannot route through, not doors present: a slab stays
    // in the geometry until its door is fully open, so the face total no longer
    // moves on the step passability changes and a field keyed to it would go
    // stale with every robot still steering around an open doorway.
    const int shutDoors = doors ? doors->shutCount() : 0;
    if (state.pathSegment != player.segment || state.pathDoorCount != shutDoors) {
        rebuildPathField(level, state, player.segment, doors);
    }

    CollisionConfig collision;
    collision.scrapeDamping = 1.0f;

    // Asked once for the whole step, not once per robot: it is a property of
    // where the player is standing, identical for every robot looking at them.
    const PlayerVisibility visibility =
        playerVisibility(level, combat, weapons, player.position, player.segment, doors, props,
                         occluders);

    for (int ri = 0; ri < state.count; ++ri) {
        Robot& robot = state.robots[static_cast<size_t>(ri)];
        if (!robot.alive) continue;
        const RobotDef& def = cfg.types[static_cast<size_t>(robot.type)];

        robot.modeTime += dt;
        robot.fireCooldown = std::max(0.0f, robot.fireCooldown - dt);
        robot.burstTimer = std::max(0.0f, robot.burstTimer - dt);
        robot.specialCooldown = std::max(0.0f, robot.specialCooldown - dt);
        robot.strafeTimer -= dt;
        if (robot.strafeTimer <= 0.0f) {
            robot.strafeTimer = 0.8f + randUnit() * 0.8f;
            robot.strafeSign = randUnit() < 0.5f ? -1.0f : 1.0f;
        }

        // --- disabled ----------------------------------------------------
        //
        // Under the threshold a robot stops being a robot: it does not look,
        // path, steer or shoot. It keeps exactly the momentum it had and
        // tumbles, and it can still be finished off.
        //
        // Expressed as "skip everything else" rather than as a desired velocity
        // of zero, because steer() would read that as a request to stop and
        // brake it to a halt against the very momentum it is supposed to keep.
        const float health0 = def.health > 0.0f ? robot.health / def.health : 1.0f;
        if (health0 <= kDisabledHealthFraction) {
            if (robot.mode != RobotMode::Disabled) {
                setMode(robot, RobotMode::Disabled);
                robot.awareness = 0.0f;
                robot.burstLeft = 0;
                // A disabled robot is harmless, and that includes the wind-up
                // it was holding: it goes with the power.
                robot.windingUp = false;
                robot.windupTime = 0.0f;
                // Folded in, not left to decay: with nothing flying the robot,
                // a shove and a drift are the same thing, and the drift is
                // meant to last.
                robot.body.velocity += robot.body.knockback;
                robot.body.knockback = glm::vec3(0.0f);
                robot.tumble = glm::vec3(randSigned() * 1.1f, randSigned() * 1.1f,
                                         randSigned() * 1.6f);
            }

            robot.body.velocity += robot.body.knockback;
            robot.body.knockback = glm::vec3(0.0f);
            robot.orientation =
                glm::normalize(robot.orientation *
                               glm::quat(0.0f, robot.tumble * 0.5f * dt) + robot.orientation);
            robot.body.position += robot.body.velocity * dt;
            resolveCollision(level, robot.body, collision, dt, nullptr, doors);
            if (robot.attachmentOn) placeAttachment(robot, def);

            if (robot.health <= 0.0f) {
                knockAttachmentLoose(level, state, ri, glm::vec3(0.0f), doors);
                robot.alive = false;
                emit(events, Cue::RobotDeath, robot.body.position, robot.body.segment);
                spawnDebris(state, robot, def);
                spawnDrop(level, state, def, robot);
            }
            continue;
        }
        robot.tumble = glm::vec3(0.0f);

        // --- perception --------------------------------------------------
        const glm::vec3 toPlayer = player.position - robot.body.position;
        const float distance = glm::length(toPlayer);
        bool canSee = false;
        // No range test. Distance is not what decides whether a robot has
        // noticed you - line of sight and whether you are lit are, and the
        // mine's own bends, pillars and shut doors are what break them. A
        // radius drawn around each robot only added a second, invisible rule
        // on top, so that standing in plain view down a lit gallery was safe
        // for no reason the player could see.
        //
        // What is left is `distance > 1e-3f`, which is not a range: a robot
        // sitting exactly on the player has no direction to cast along.
        if (distance > 1e-3f) {
            const glm::vec3 dir = toPlayer / distance;
            const RayHit hit =
                raycast(level, robot.body.position, dir, distance, robot.body.segment, doors);
            // The ray stops at the first wall or shut door; if that is farther
            // than the player, nothing is in the way.
            canSee = !hit.hit || hit.distance >= distance - 0.5f;

            // And now the things that are not the mine. This is where hiding
            // behind a pillar became real: until M8 the ray above was the whole
            // test, so a column broke nothing, and an alert robot tracked the
            // player straight through it.
            //
            // The robot doing the looking and the player being looked at are
            // both left out, or every robot would be blinded by its own hull
            // and nothing would ever see anybody.
            if (canSee && occluders) {
                canSee = !occluded(*occluders, robot.body.position, player.position,
                                   OccluderId{OccluderKind::Robot, ri},
                                   OccluderId{OccluderKind::Ship, -1});
            }
        }

        // Line of sight is necessary but no longer sufficient. A robot that has
        // never seen the player cannot pick them out of an unlit stretch of
        // mine; one that is already on to them keeps hunting whether or not
        // the player is standing in the light. That asymmetry is the whole
        // mechanic: darkness hides you until something gives you away, and
        // once it has, going dark again does not call the robots off - you have
        // to break line of sight and wait out their awareness.
        const bool alreadyOnToThem = robot.awareness > 0.0f;
        if (canSee && (visibility.visible || alreadyOnToThem)) {
            robot.awareness = 1.0f;
            robot.lastKnown = player.position;
            robot.hasLastKnown = true;
        } else {
            // Decays over about three seconds, so a robot keeps hunting for a
            // moment after you break line of sight rather than forgetting you
            // the instant a wall gets between you.
            robot.awareness = std::max(0.0f, robot.awareness - dt / 3.0f);
        }

        // --- transitions -------------------------------------------------
        const float healthFraction = def.health > 0.0f ? robot.health / def.health : 1.0f;
        const bool hurt = healthFraction <= def.retreatThreshold;

        switch (robot.mode) {
            case RobotMode::Idle:
                if (robot.awareness > 0.0f) {
                    setMode(robot, RobotMode::Alert);
                    // The one transition worth hearing. In a mine with no
                    // ambient light, a robot waking up somewhere behind you is
                    // information you cannot get any other way.
                    emit(events, Cue::RobotAlert, robot.body.position, robot.body.segment);
                }
                break;
            case RobotMode::Alert:
                if (robot.awareness <= 0.0f) {
                    setMode(robot, RobotMode::Idle);
                } else if (robot.modeTime > 0.4f) {
                    setMode(robot, canSee && distance < def.attackRange ? RobotMode::Attack
                                                                        : RobotMode::Chase);
                }
                break;
            case RobotMode::Chase:
                if (robot.awareness <= 0.0f) {
                    setMode(robot, RobotMode::Idle);
                } else if (hurt) {
                    setMode(robot, RobotMode::Retreat);
                } else if (canSee && distance < def.attackRange) {
                    setMode(robot, RobotMode::Attack);
                }
                break;
            case RobotMode::Attack:
                if (hurt) {
                    setMode(robot, RobotMode::Retreat);
                } else if (!canSee || distance > def.attackRange * 1.15f) {
                    setMode(robot, robot.awareness > 0.0f ? RobotMode::Chase : RobotMode::Idle);
                } else if (robot.burstLeft == 0 && robot.burstTimer > 0.0f &&
                           randUnit() > def.aggression) {
                    // Having just finished a burst, an unaggressive robot backs
                    // off around the corner instead of standing and trading.
                    // This is the behaviour that makes them read as cautious
                    // rather than as turrets.
                    setMode(robot, RobotMode::Retreat);
                }
                break;
            case RobotMode::Retreat:
                if (robot.modeTime > def.retreatTime) {
                    setMode(robot, robot.awareness > 0.0f ? RobotMode::Chase : RobotMode::Idle);
                }
                break;
            case RobotMode::Disabled:
            case RobotMode::Count:
                break;
        }

        // --- behaviour ---------------------------------------------------
        glm::vec3 desired(0.0f);
        glm::vec3 facing = robot.orientation * glm::vec3(0.0f, 0.0f, -1.0f);

        switch (robot.mode) {
            case RobotMode::Idle: {
                desired = glm::vec3(0.0f);
                // A slow drift turn, so an idle robot is not a statue.
                const glm::quat spin =
                    glm::angleAxis(0.35f * dt, glm::vec3(0.0f, 1.0f, 0.0f));
                robot.orientation = glm::normalize(spin * robot.orientation);
                break;
            }
            case RobotMode::Alert: {
                if (robot.hasLastKnown) {
                    faceTowards(robot, robot.lastKnown - robot.body.position, def.turnRate, dt);
                }
                break;
            }
            case RobotMode::Chase: {
                glm::vec3 waypoint;
                const bool sameSegment = robot.body.segment == player.segment;
                const glm::vec3 goal = sameSegment || !nextWaypoint(level, state,
                                                                    robot.body.segment, doors,
                                                                    &waypoint)
                                           ? (robot.hasLastKnown ? robot.lastKnown
                                                                 : player.position)
                                           : waypoint;
                const glm::vec3 to = goal - robot.body.position;
                if (glm::length(to) > 0.5f) {
                    desired = glm::normalize(to);
                    faceTowards(robot, to, def.turnRate, dt);
                }
                break;
            }
            case RobotMode::Attack: {
                faceTowards(robot, toPlayer, def.turnRate, dt);
                facing = robot.orientation * glm::vec3(0.0f, 0.0f, -1.0f);

                // Hold a working distance and slide sideways, so it is a moving
                // target rather than a stationary one.
                const glm::vec3 right = robot.orientation * glm::vec3(1.0f, 0.0f, 0.0f);
                const float preferred = def.attackRange * (0.35f + 0.4f * (1.0f - def.aggression));
                const float closing = distance - preferred;
                if (distance > 1e-3f) {
                    desired = (toPlayer / distance) * std::clamp(closing / 12.0f, -1.0f, 1.0f) +
                              right * robot.strafeSign * 0.6f;
                    if (glm::length(desired) > 1.0f) desired = glm::normalize(desired);
                }

                if (robot.burstLeft <= 0 && robot.burstTimer <= 0.0f) {
                    robot.burstLeft = def.burstCount;
                }
                if (robot.burstLeft > 0 && robot.fireCooldown <= 0.0f && canSee) {
                    // Lead the player's velocity. Early shots in a burst are
                    // given extra spread so a burst walks onto the target
                    // instead of all landing or all missing.
                    const float flightTime =
                        def.projectileSpeed > 0.0f ? distance / def.projectileSpeed : 0.0f;
                    const glm::vec3 aimPoint =
                        player.position + player.velocity * (def.lead * flightTime);
                    glm::vec3 aim = glm::normalize(aimPoint - robot.body.position);

                    const float shotInBurst =
                        static_cast<float>(def.burstCount - robot.burstLeft);
                    const float spread =
                        def.spread * (1.6f - 0.5f * shotInBurst /
                                                  std::max(1.0f, static_cast<float>(def.burstCount)));
                    aim = glm::normalize(aim + glm::vec3(randSigned(), randSigned(), randSigned()) *
                                                   spread);

                    const int slot = combat.projectiles.spawn();
                    if (slot >= 0) {
                        Projectile& p = combat.projectiles.items[static_cast<size_t>(slot)];
                        p.kind = ProjectileKind::Laser;
                        p.position = robot.body.position + aim * (def.radius + 0.6f);
                        p.velocity = aim * def.projectileSpeed;
                        p.color = def.color;
                        p.segment = robot.body.segment;
                        p.radius = 0.3f;
                        p.damage = def.projectileDamage;
                        p.impactImpulse = def.projectileImpulse;
                        p.lifetime = 4.0f;
                        p.lightRadius = 14.0f;
                        p.lightIntensity = 1.8f;
                        p.owner = ri;

                        // The same flash the player's guns make. In a black
                        // corridor this is often the first thing that tells the
                        // player where a robot is - before the bolt, before the
                        // eyes.
                        spawnEffect(combat, p.position, p.segment, def.color, 0.9f, 0.05f, 21.0f,
                                    4.5f);
                        emit(events, Cue::RobotFire, p.position, p.segment);
                    }

                    --robot.burstLeft;
                    robot.fireCooldown = def.fireInterval;
                    if (robot.burstLeft == 0) robot.burstTimer = def.burstPause;
                }
                break;
            }
            case RobotMode::Retreat: {
                // Back off towards where it came from, still facing the player
                // so it can keep shooting on the way out.
                faceTowards(robot, toPlayer, def.turnRate * 0.7f, dt);
                if (distance > 1e-3f) desired = -(toPlayer / distance);
                break;
            }
            case RobotMode::Disabled:
            case RobotMode::Count:
                break;
        }

        steer(robot, desired, def, dt);
        // Steering drives `velocity` only. The knockback channel is what a
        // blast put there, and the robot flies it off rather than deleting it:
        // it reads as thrusters fighting the shove.
        robot.body.knockback *= std::exp(-kKnockbackDamping * dt);
        robot.body.position += robot.body.motion() * dt;
        // Robots are Bodies, so they cannot leave the mine any more than the
        // player can. No separate containment logic, and no separate bugs.
        // The same shut doors stop them too.
        resolveCollision(level, robot.body, collision, dt, nullptr, doors);

        // --- attachment and special attack --------------------------------
        //
        // After the move, so the attachment and the muzzle are where they are
        // drawn this step. A wind-up starts only in Attack with the player in
        // sight; once started it runs whatever the robot does next, and it
        // always ends in a shot. See SpecialAttackDef.
        if (robot.attachmentOn) {
            placeAttachment(robot, def);
            const SpecialAttackDef& sp = def.special;
            if (sp.enabled && !robot.windingUp && robot.specialCooldown <= 0.0f &&
                robot.mode == RobotMode::Attack && canSee) {
                robot.windingUp = true;
                robot.windupTime = 0.0f;
                robot.pulseTimer = 0.0f;
            }
            if (robot.windingUp) {
                robot.windupTime += dt;
                const float k = sp.windup > 0.0f ? std::min(1.0f, robot.windupTime / sp.windup)
                                                 : 1.0f;
                robot.pulseTimer -= dt;
                if (robot.pulseTimer <= 0.0f) {
                    robot.pulseTimer = sp.pulseStart + (sp.pulseEnd - sp.pulseStart) * k;
                    emit(events, Cue::SpecialWindup, robot.muzzle, robot.body.segment,
                         0.35f + 0.65f * k);
                }
                if (robot.windupTime >= sp.windup) {
                    fireSpecial(level, def, robot, ri, combat, player, weapons, canSee, doors,
                                events, props, state);
                }
            }
        }

        // --- death -------------------------------------------------------
        if (robot.health <= 0.0f) {
            knockAttachmentLoose(level, state, ri, glm::vec3(0.0f), doors);
            robot.alive = false;
            emit(events, Cue::RobotDeath, robot.body.position, robot.body.segment);
            spawnDebris(state, robot, def);
            spawnDrop(level, state, def, robot);

            const int slot = combat.effects.spawn();
            if (slot >= 0) {
                Effect& e = combat.effects.items[static_cast<size_t>(slot)];
                e.position = robot.body.position;
                e.segment = robot.body.segment;
                e.color = glm::vec3(1.0f, 0.7f, 0.35f);
                e.size = 7.0f;
                e.lifetime = 0.55f;
                e.lightRadius = 30.0f;
                e.lightIntensity = 8.0f;
            }
        }
    }

    // --- debris ----------------------------------------------------------
    for (Debris& d : state.debris) {
        if (!d.alive) continue;
        d.age += dt;
        if (d.age >= d.lifetime) {
            d.alive = false;
            continue;
        }
        d.body.knockback *= std::exp(-kKnockbackDamping * dt);
        d.body.position += d.body.motion() * dt;
        resolveCollision(level, d.body, collision, dt, nullptr, doors);
        // Bleed speed so chunks settle rather than pinballing forever.
        d.body.velocity *= std::exp(-1.4f * dt);
    }

    // --- loose attachments ---------------------------------------------
    //
    // Like debris, but kept: it tumbles off, slows, and stays where it lands.
    // The spin bleeds off with the speed, so it comes to rest rather than
    // turning forever in place.
    for (LooseAttachment& loose : state.looseAttachments) {
        if (!loose.alive) continue;
        loose.body.knockback *= std::exp(-kKnockbackDamping * dt);
        loose.body.position += loose.body.motion() * dt;
        resolveCollision(level, loose.body, collision, dt, nullptr, doors);
        loose.body.velocity *= std::exp(-1.1f * dt);
        loose.spin *= std::exp(-1.1f * dt);
        loose.orientation = glm::normalize(
            loose.orientation + loose.orientation * glm::quat(0.0f, loose.spin * 0.5f * dt));
    }
}

} // namespace dvk::sim
