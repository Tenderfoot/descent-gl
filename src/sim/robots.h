#pragma once

#include "sim/body.h"
#include "sim/events.h"

#include <array>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <string>
#include <vector>

namespace dvk {
struct Level;
}

namespace dvk::sim {

struct CombatState;
struct WeaponConfig;
struct SolidDoors;

// What a robot is doing. Every transition is reported in the ImGui readout,
// because a state machine you cannot watch is a state machine you cannot tune.
enum class RobotMode : uint8_t {
    Idle,      // drifting, slow turn, has not noticed anything
    Alert,     // heard or saw something; turning towards where it was
    Chase,     // pathing towards the player through the segment graph
    Attack,    // in line of sight and in range: face, fire a burst, strafe
    Retreat,   // hurt, or just finished a burst: back off, wait, return
    Disabled,  // under kDisabledHealthFraction: drifting, blind, harmless
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(RobotMode::Count)> kRobotModeNames{
    "idle", "alert", "chase", "attack", "retreat", "disabled"};

// Per-type data, from assets/config/robots.json.
struct RobotDef {
    std::string name;
    std::string model;
    float health = 30.0f;
    float radius = 2.0f;
    // How hard it is to shove. An impulse is divided by this, so a drone gets
    // thrown across a room by the same blast that rocks a brute.
    float mass = 1.0f;
    float speed = 14.0f;           // units/s
    float acceleration = 26.0f;
    float turnRate = 2.2f;         // rad/s
    float sightRange = 90.0f;
    float attackRange = 60.0f;
    // 0 pushes in to point blank, 1 keeps its distance. Also how readily it
    // leaves Idle.
    float aggression = 0.6f;
    float retreatThreshold = 0.3f; // fraction of health
    float retreatTime = 1.6f;
    float projectileSpeed = 85.0f;
    float projectileDamage = 6.0f;
    float projectileImpulse = 5.0f;   // the punch its bolts carry
    float fireInterval = 0.35f;    // between shots inside a burst
    int burstCount = 3;
    float burstPause = 1.4f;
    float spread = 0.05f;          // radians
    // How much of the player's velocity the aim leads by. Below 1 so a moving
    // player is still hard to hit, which is what makes strafing worth doing.
    float lead = 0.65f;
    glm::vec3 color{1.0f, 0.4f, 0.3f};
    glm::vec3 eyeColor{1.0f, 0.25f, 0.15f};
    float eyeSize = 0.32f;
    // Local offsets of the emissive eye billboards, in model space.
    //
    // THESE MUST SIT OUTSIDE THE ROBOT'S OWN HULL. The eyes are drawn as
    // camera-facing quads in the unlit pass, which is depth-tested against a
    // depth prepass that draws every robot mesh unconditionally. An offset
    // inside the hull is therefore occluded by the robot's own body - and in
    // an unlit corridor that body is invisible, so the eyes vanish behind
    // nothing at all, which is the exact opposite of what they are for.
    //
    // Place each one on the surface the eye belongs to and push it out along
    // that surface's normal by about half of `eyeSize` (the quad's HALF
    // extent, so the sprite is 2 * eyeSize across). That clearance is what
    // makes the whole quad readable head-on; past roughly 30 degrees off axis
    // the quad starts to clip into the hull again and reads as a recessed
    // lamp, which is the intended look.
    std::vector<glm::vec3> eyes;
    // Chance of each drop, in order: nothing, energy, shield.
    // The reactor is a robot that does not move: high health, slow bolts,
    // stationary. Reusing the robot path gives it pathing it never uses, but
    // also gives it shadow casting, damage, death and debris for free - and
    // one code path that is exercised by every other robot in the level.
    bool isReactor = false;
    float dropNothing = 0.35f;
    float dropEnergy = 0.35f;
    float dropShield = 0.30f;
};

struct RobotConfig {
    std::vector<RobotDef> types;

    const RobotDef* find(const std::string& name) const {
        for (const RobotDef& d : types) {
            if (d.name == name) return &d;
        }
        return nullptr;
    }
    int indexOf(const std::string& name) const {
        for (size_t i = 0; i < types.size(); ++i) {
            if (types[i].name == name) return static_cast<int>(i);
        }
        return -1;
    }

    static RobotConfig defaults();
};

RobotConfig loadRobotConfig(const std::string& path);
bool saveRobotConfig(const std::string& path, const RobotConfig& cfg);

struct Robot {
    bool alive = false;
    int type = 0;
    Body body;
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    float health = 0.0f;
    // Copied from the type at spawn. Carried on the instance because the code
    // that shoves a robot - splash damage, a bolt landing - has the RobotState
    // but not the RobotConfig.
    float mass = 1.0f;

    RobotMode mode = RobotMode::Idle;
    RobotMode previousMode = RobotMode::Idle;
    float modeTime = 0.0f;
    // 1 while it can see the player, decaying to 0 over a few seconds once it
    // cannot. Awareness, not line of sight, is what drives the state machine,
    // so a robot keeps hunting for a moment after you break line of sight.
    float awareness = 0.0f;
    glm::vec3 lastKnown{0.0f};
    bool hasLastKnown = false;

    float fireCooldown = 0.0f;
    int burstLeft = 0;
    float burstTimer = 0.0f;
    // Which way it slides while attacking, flipped periodically so it does not
    // simply sit still and trade shots.
    float strafeSign = 1.0f;
    float strafeTimer = 0.0f;
    // The aimless spin a disabled robot picks up, in its local frame. Chosen
    // once when it goes under and kept, so it tumbles steadily rather than
    // jittering to a new random attitude every step.
    glm::vec3 tumble{0.0f};
    int homeSegment = 0;
};

enum class DropKind : uint8_t { Energy, Shield };

struct Drop {
    bool alive = false;
    DropKind kind = DropKind::Energy;
    glm::vec3 position{0.0f};
    int segment = 0;
    float age = 0.0f;
};

struct Debris {
    bool alive = false;
    Body body;
    glm::vec3 color{1.0f};
    float age = 0.0f;
    float lifetime = 2.5f;
};

inline constexpr int kMaxRobots = 32;
inline constexpr int kMaxDrops = 24;
inline constexpr int kMaxDebris = 48;
inline constexpr int kMaxPathSegments = 128;

struct RobotState {
    std::array<Robot, kMaxRobots> robots{};
    std::array<Drop, kMaxDrops> drops{};
    std::array<Debris, kMaxDebris> debris{};
    int count = 0;

    // Distance in portals from every segment to the player's. One breadth-first
    // search serves every robot, because they all chase the same thing; a robot
    // steers towards whichever neighbour has a lower number. Rebuilt only when
    // the player changes segment.
    std::array<int16_t, kMaxPathSegments> distanceToPlayer{};
    int pathSegment = -1;      // the player segment the field was built for
    int pathValidCount = 0;
    // How many shut door faces the field was built against, so a door opening
    // or closing rebuilds it: a locked door is a wall to the path search.
    int pathDoorCount = -1;

    int aliveCount() const {
        int n = 0;
        for (const Robot& r : robots) {
            if (r.alive) ++n;
        }
        return n;
    }
};

// Spawns robots from the level's object list: any object whose type names a
// robot definition becomes one. Everything else stays static scenery.
void spawnRobots(const Level& level, const RobotConfig& cfg, RobotState& out);

// True if this object's type is a robot rather than scenery, so the renderer
// knows not to draw it as a static prop.
bool objectIsRobot(const RobotConfig& cfg, const std::string& type);

// One fixed step of every robot. `doors` is the shut-door set for this step:
// a robot can neither see, path nor fly through a shut door. Unlocked doors
// open for robots, so the path search only treats a LOCKED shut door as a
// wall; a shut door with no key is walked up to and opened.
// `events` is the sound queue and may be null. See sim/events.h.
void stepRobots(const Level& level, const RobotConfig& cfg, const WeaponConfig& weapons,
                RobotState& state, CombatState& combat, Body& player, float dt,
                const SolidDoors* doors = nullptr, EventQueue* events = nullptr);

} // namespace dvk::sim
