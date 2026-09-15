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
    drone.sightRange = 80.0f;
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
    gunner.sightRange = 110.0f;
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
    brute.sightRange = 95.0f;
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
    cfg.types.push_back(brute);

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
    reactor.sightRange = 130.0f;
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
            get(tj, "sightRange", def.sightRange);
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
        tj["sightRange"] = def.sightRange;
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
        robot.orientation = obj.orientation;
        robot.health = obj.health > 0.0f ? obj.health : def.health;
        robot.energy = def.energy;
        robot.mass = def.mass;
        robot.homeSegment = obj.segment;
    }
    log::info("{} robots spawned", out.count);
}

namespace {

// A portal a robot cannot get through: a shut door it has no way to open.
// Robots carry no keys, so a keyed door is a wall until the player opens it;
// an unkeyed door opens for anything that comes near, so it is a corridor.
bool portalLockedShut(const Level& level, const SolidDoors* doors, int segment,
                      const SideData& sd) {
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
    state.pathDoorCount = doors ? doors->count : 0;
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

void spawnDrop(const Level& level, RobotState& state, const RobotDef& def, const Robot& robot) {
    const float roll = randUnit() * (def.dropNothing + def.dropEnergy + def.dropShield);
    if (roll < def.dropNothing) return;

    for (Drop& drop : state.drops) {
        if (drop.alive) continue;
        drop = Drop{};
        drop.alive = true;
        drop.kind = roll < def.dropNothing + def.dropEnergy ? DropKind::Energy : DropKind::Shield;
        drop.position = robot.body.position;
        drop.segment = robot.body.segment;
        (void)level;
        return;
    }
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
            d.body.velocity =
                glm::vec3(randSigned(), randSigned(), randSigned()) * 18.0f + robot.body.velocity;
            d.color = def.color;
            d.lifetime = 1.8f + randUnit() * 1.4f;
            break;
        }
    }
}

} // namespace

void stepRobots(const Level& level, const RobotConfig& cfg, const WeaponConfig& weapons,
                RobotState& state, CombatState& combat, Body& player, float dt,
                const SolidDoors* doors, EventQueue* events, const PropState* props) {
    (void)weapons;

    const int shutDoors = doors ? doors->count : 0;
    if (state.pathSegment != player.segment || state.pathDoorCount != shutDoors) {
        rebuildPathField(level, state, player.segment, doors);
    }

    CollisionConfig collision;
    collision.scrapeDamping = 1.0f;

    // Asked once for the whole step, not once per robot: it is a property of
    // where the player is standing, identical for every robot looking at them.
    const PlayerVisibility visibility =
        playerVisibility(level, combat, weapons, player.position, player.segment, doors, props);

    for (int ri = 0; ri < state.count; ++ri) {
        Robot& robot = state.robots[static_cast<size_t>(ri)];
        if (!robot.alive) continue;
        const RobotDef& def = cfg.types[static_cast<size_t>(robot.type)];

        robot.modeTime += dt;
        robot.fireCooldown = std::max(0.0f, robot.fireCooldown - dt);
        robot.burstTimer = std::max(0.0f, robot.burstTimer - dt);
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

            if (robot.health <= 0.0f) {
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
        if (distance < def.sightRange && distance > 1e-3f) {
            const glm::vec3 dir = toPlayer / distance;
            const RayHit hit =
                raycast(level, robot.body.position, dir, distance, robot.body.segment, doors);
            // The ray stops at the first wall or shut door; if that is farther
            // than the player, nothing is in the way.
            canSee = !hit.hit || hit.distance >= distance - 0.5f;
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

        // --- death -------------------------------------------------------
        if (robot.health <= 0.0f) {
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

    // --- drops -----------------------------------------------------------
    for (Drop& drop : state.drops) {
        if (!drop.alive) continue;
        drop.age += dt;
        if (glm::length(drop.position - player.position) < player.radius + 2.2f) {
            // M7 turns these into real pickups. For now touching one removes
            // it, which is enough to show that the drop table works.
            drop.alive = false;
        }
    }
}

} // namespace dvk::sim
