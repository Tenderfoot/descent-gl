#pragma once

#include "game/level.h"
#include "sim/body.h"
#include "sim/occluders.h"
#include "sim/events.h"
#include "sim/proxy.h"
#include "sim/weapons.h"

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
    Disabled,  // under kDisabledHealthFraction: drifting, blind, harmless
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(RobotMode::Count)> kRobotModeNames{
    "idle", "alert", "chase", "attack", "disabled"};

// A second model hung on a robot: the brute's shoulder railgun, and whatever
// comes after it. It is drawn, lit and shadowed as its own mesh, and it has a
// hitbox of its own, so it can be aimed at. A direct hit from `breaksOn` knocks
// it loose - it tumbles off and stays where it lands - and whatever special
// attack it carried goes with it.
struct AttachmentDef {
    // Names a mesh in assets/models/. Empty means the type has no attachment.
    std::string model;
    // Where the model's origin sits, in the robot's model space.
    glm::vec3 mount{0.0f};
    // The hitbox, in the attachment's own space. resolveRobotAttachments sets
    // it from the model's bounds when the config is loaded, so the box the
    // player shoots at is the thing they can see.
    ProxyVolume volume = sphereProxy(1.0f);
    // The player weapon whose direct hit knocks it off. Only a direct hit:
    // splash never does, and any other weapon hitting it hits the robot.
    int breaksOn = WeaponRailgun;
    // What knocking it off costs the robot, as a fraction of its full health.
    // The reward for the accurate shot: it takes the special attack away and
    // hurts more than the same round in the hull would. It can finish a robot
    // that was already worn down.
    float breakDamage = 0.5f;

    bool present() const { return !model.empty(); }
};

// How a special attack's shot travels, once the wind-up is over.
//
//   Hitscan     one trace from the muzzle, like the player's railgun.
//   Projectile  one bolt from the muzzle, swept through the mine like any
//               other robot fire.
enum class SpecialDelivery : uint8_t { Hitscan, Projectile };

inline constexpr std::array<const char*, 2> kSpecialDeliveryNames{"hitscan", "projectile"};

// A telegraphed attack, fired from a robot's attachment and lost with it.
//
// It runs alongside the robot's ordinary bursts. It starts only in Attack with
// the player in sight, winds up for `windup` seconds - an orb at the muzzle and
// a light there, both growing from nothing, and a pulse that repeats faster and
// louder - and then always fires: at the player if the robot can see them, at
// where it last saw them if not. There is no lead and no spread. The wind-up is
// the warning, and the answers to it are to break the attachment off, to get
// something solid in the way, or to take the hit.
struct SpecialAttackDef {
    bool enabled = false;
    SpecialDelivery delivery = SpecialDelivery::Hitscan;
    // Where the shot leaves, in the ATTACHMENT's model space.
    glm::vec3 muzzle{0.0f};
    float windup = 5.0f;
    // After a shot, before the next wind-up may start.
    float cooldown = 4.0f;
    // What lands on the player. The impulse is a velocity change at mass 1,
    // into the ship's knockback channel, so it is the knockback damping that
    // bleeds it off.
    float damage = 30.0f;
    float impulse = 20.0f;
    glm::vec3 color{1.0f};
    // The orb and its light at the end of the wind-up.
    float orbSize = 0.9f;
    float lightRadius = 24.0f;
    float lightIntensity = 4.5f;
    // Seconds between wind-up pulses at the start and at the end of it.
    float pulseStart = 0.55f;
    float pulseEnd = 0.12f;

    // Hitscan only: how far it reaches, and its tracer.
    float range = 400.0f;
    float beamWidth = 0.3f;
    float beamTime = 0.35f;

    // Projectile only.
    float projectileSpeed = 60.0f;
    float projectileRadius = 0.5f;
    float projectileLifetime = 6.0f;
    float splashRadius = 0.0f;
    float splashDamage = 0.0f;
    float splashImpulse = 0.0f;
    // Steers after the player at up to `turnRate` rad/s for its whole life.
    // Tuned below what the ship can out-turn, so a hard strafe or a tight
    // corner makes it overshoot: flying is an answer as well as shooting.
    bool homing = false;
    float turnRate = 1.4f;
    // The player's fire can hit it, and it goes off where it was hit - with
    // its full blast, which hurts whatever is near it, robots included.
    bool shootable = false;
};

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

    // A separate model hung on the robot, and the special attack it carries.
    // See AttachmentDef and SpecialAttackDef. A type with neither is an
    // ordinary robot.
    AttachmentDef attachment;
    SpecialAttackDef special;

    float dropNothing = 0.35f;
    float dropEnergy = 0.35f;
    float dropShield = 0.30f;

    // What a Static Charge finish drops besides its energy: the ammunition
    // this type carries, one pickup of each, scattered around where it died.
    // The chances above do not apply to a charge finish. See spawnDrop.
    std::vector<PickupKind> chargeDrops;
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

// What landed the last damage a robot took, for the drop it leaves if that
// damage is what killed it: a Static Charge finish resupplies the player, a
// player missile's (impact or blast) repairs their shields. See spawnDrop.
enum class LastHit : uint8_t { Other, Charge, Missile };

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

    LastHit lastHit = LastHit::Other;

    // The attachment, for a type that has one. See AttachmentDef.
    bool attachmentOn = false;
    // Copied from the type at spawn, for the same reason `mass` is: the shot
    // that breaks it off is judged by stepCombat, which has the RobotState
    // but not the RobotConfig.
    int attachmentBreaksOn = -1;
    // Health lost when it is knocked off: AttachmentDef::breakDamage of the
    // type's full health, copied at spawn for the same reason.
    float attachmentBreakDamage = 0.0f;
    // The attachment model's origin in world space, and its hitbox, placed
    // from the robot's own transform every step. Kept on the robot because
    // the renderer draws it here and the combat trace hits it here, and
    // neither has the robot config to work it out.
    glm::vec3 attachmentOrigin{0.0f};
    PlacedProxy attachmentProxy;

    // The special attack. See SpecialAttackDef. A wind-up only ends by
    // firing, or by the attachment or the robot being lost.
    bool windingUp = false;
    float windupTime = 0.0f;       // seconds into the current wind-up
    float specialCooldown = 0.0f;
    float pulseTimer = 0.0f;
    // Where the shot will leave from, in world space; the orb and its light
    // sit here too. Kept up to date for the same reason as attachmentOrigin.
    glm::vec3 muzzle{0.0f};
};

// An attachment that has been shot off, or whose robot died under it. It
// tumbles away and settles, and stays where it ends up: a wreck the player
// made, not an effect.
struct LooseAttachment {
    bool alive = false;
    int type = 0;            // the robot type it came off, for its model
    Body body;               // at the attachment's own origin
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 spin{0.0f};    // rad/s in its own frame, bled off as it settles
};

// What a robot leaves behind. A pickup the level did not place: stepGame
// collects it alongside the authored ones, with the same effect, sound and
// script hooks, and it is drawn as the same sprite. It stays until it is taken.
struct Drop {
    bool alive = false;
    PickupKind kind = PickupKind::Energy;
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
// Every robot finished by the Static Charge at once, each leaving its energy
// and up to two kinds of ammunition.
inline constexpr int kMaxDrops = kMaxRobots * 3;
inline constexpr int kMaxDebris = 48;
inline constexpr int kMaxPathSegments = 128;

struct RobotState {
    std::array<Robot, kMaxRobots> robots{};
    std::array<Drop, kMaxDrops> drops{};
    std::array<Debris, kMaxDebris> debris{};
    // Indexed by robot slot: a robot has at most one attachment to lose, so
    // the slot it came off is the one it lives in and nothing is allocated.
    std::array<LooseAttachment, kMaxRobots> looseAttachments{};
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

// Knocks a robot's attachment loose: it leaves the robot as a LooseAttachment
// carrying the robot's motion plus `push`, and any wind-up it was holding goes
// with it. Does nothing to a robot with no attachment on. Public because the
// shot that does it is the player's, and that is stepCombat's.
void knockAttachmentLoose(const Level& level, RobotState& state, int slot, const glm::vec3& push,
                          const SolidDoors* doors = nullptr);

} // namespace dvk::sim
