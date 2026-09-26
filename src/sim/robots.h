#pragma once

#include "sim/body.h"
#include "sim/occluders.h"
#include "sim/events.h"
#include "sim/proxy.h"

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
struct PropState;

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
//
// THERE IS NO `sightRange`, and putting one back is a regression. What a robot
// can see is line of sight and whether the player is lit; the mine's own
// bends, pillars and shut doors are what limit it. A per-type radius was a
// second, invisible rule sitting on top of those, and it meant standing in
// plain view down a lit gallery was safe for no reason the player could read.
// `attackRange` below is a different thing and still exists: how close a robot
// has to be before it shoots at what it has already noticed.
struct RobotDef {
    std::string name;
    std::string model;
    float health = 30.0f;
    // Reserved for future firing/recharge behaviour. Until that exists every
    // robot spawns full and nothing drains this resource.
    float energy = 100.0f;
    float radius = 2.0f;
    // How hard it is to shove. An impulse is divided by this, so a drone gets
    // thrown across a room by the same blast that rocks a brute.
    float mass = 1.0f;
    float speed = 14.0f;           // units/s
    float acceleration = 26.0f;
    float turnRate = 2.2f;         // rad/s
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

    // The shoulder railgun: a second, separate model hung on the robot, with a
    // hitbox of its own. While it is attached the robot charges it for
    // `railgunChargeTime` alongside its ordinary bursts - a growing orb and a
    // growing light at the muzzle, and a rising pulse to hear it by - and then
    // fires one hitscan shot at the player. A charge that has started always
    // fires: at the player if it can see them, at where it last saw them if
    // not. The player's own railgun landing on the gun knocks it loose, and
    // from then on the robot has only its ordinary bursts.
    bool railgun = false;
    std::string railgunModel = "railgun";
    // Where the gun model's origin sits, in the robot's model space, and
    // where its shot leaves, in the GUN's model space.
    glm::vec3 railgunMount{2.2f, 2.0f, -1.1f};
    glm::vec3 railgunMuzzle{0.0f, 0.0f, -2.75f};
    // The gun's hitbox, in the gun's model space. These are the bounds of
    // assets/models/railgun.obj; resolveRobotAttachments overwrites them from
    // the model itself, so a reshaped gun is not shot at through its old box.
    ProxyVolume railgunVolume = boxProxy(glm::vec3(-0.5f, -0.45f, -2.5f),
                                         glm::vec3(0.5f, 0.7f, 2.5f));
    float railgunChargeTime = 5.0f;
    // After a shot, before the next charge may start.
    float railgunCooldown = 4.0f;
    float railgunDamage = 32.0f;
    // A velocity change at mass 1, like every other impulse. Into the ship's
    // knockback channel, so it is the knockback damping that bleeds it off.
    float railgunImpulse = 30.0f;
    float railgunRange = 400.0f;
    glm::vec3 railgunColor{0.55f, 0.8f, 1.0f};
    // The orb and its light at full charge; both grow from nothing.
    float railgunOrbSize = 0.9f;
    float railgunLightRadius = 24.0f;
    float railgunLightIntensity = 4.5f;
    // Seconds between charge pulses at the start and at the end of a charge.
    float railgunPulseStart = 0.55f;
    float railgunPulseEnd = 0.12f;

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
    float energy = 0.0f;
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

    // Whether the last damage it took came from the Static Charge. A robot the
    // Static Charge finishes always drops energy; see spawnDrop.
    bool lastHitByCharge = false;

    // The shoulder railgun, for a type that has one. See RobotDef::railgun.
    bool gunAttached = false;
    // Seconds into the current charge, and whether one is running at all. A
    // charge only ends by firing, or by the gun or the robot being lost.
    bool charging = false;
    float chargeTime = 0.0f;
    float chargeCooldown = 0.0f;
    float chargePulseTimer = 0.0f;
    // Where the shot will leave from, in world space. Kept up to date every
    // step while the gun is on, because the renderer draws the orb here and
    // the light sits here, and neither has the robot config to work it out.
    glm::vec3 muzzle{0.0f};
    // The gun model's origin in world space, for the same reason, and so the
    // gun comes loose exactly where it was drawn.
    glm::vec3 gunOrigin{0.0f};
    // The gun's hitbox, placed from the robot's own transform every step.
    PlacedProxy gunProxy;
};

// A shoulder railgun that has been shot off, or whose robot died under it. It
// tumbles away and settles, and stays where it ends up: a wreck the player
// made, not an effect.
struct LooseGun {
    bool alive = false;
    int type = 0;            // the robot type it came off, for its model
    Body body;               // at the gun's own origin
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 spin{0.0f};    // rad/s in its own frame, bled off as it settles
};

enum class DropKind : uint8_t { Energy, Shield };

// What a robot leaves behind. A pickup the level did not place: stepGame
// collects it alongside the authored ones, with the same effect, sound and
// script hooks, and it is drawn as the same sprite. It stays until it is taken.
struct Drop {
    bool alive = false;
    DropKind kind = DropKind::Energy;
    glm::vec3 position{0.0f};
    int segment = 0;
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
    // Indexed by robot slot: a robot has at most one gun to lose, so the slot
    // it came off is the one it lives in and nothing has to be allocated.
    std::array<LooseGun, kMaxRobots> looseGuns{};
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
// `props` is the level's moving lights, and is what lets a robot see the player
// standing under one. Null means the caller has none, not that they do not
// count - see playerVisibility.
// `occluders` is what stands between a robot and the player besides the mine -
// pillars, other robots, the floating lights. Null keeps the pre-M8 behaviour,
// where only walls and shut doors broke line of sight and a column was a
// decoration you could be shot through.
void stepRobots(const Level& level, const RobotConfig& cfg, const WeaponConfig& weapons,
                RobotState& state, CombatState& combat, Body& player, float dt,
                const SolidDoors* doors = nullptr, EventQueue* events = nullptr,
                const PropState* props = nullptr, const OccluderSet* occluders = nullptr);

// Knocks a robot's shoulder railgun loose: the gun leaves the robot as a
// LooseGun carrying the robot's motion plus `push`, and any charge it was
// holding is gone with it. Does nothing to a robot with no gun on. Public
// because the shot that does it is the player's, and that is stepCombat's.
void knockGunLoose(const Level& level, RobotState& state, int slot, const glm::vec3& push,
                   const SolidDoors* doors = nullptr);

} // namespace dvk::sim
