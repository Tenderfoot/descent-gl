#pragma once

#include <array>
#include <cstdint>
#include <glm/vec3.hpp>

namespace dvk::sim {

// What the simulation says happened, so something else can make a noise about
// it.
//
// The rule this file exists to keep is the audio equivalent of "sim never
// touches GL": the simulation does not know a sound system exists. It reports
// events - a gun fired, here, this loud - and the audio layer decides which
// clip that is, whether a device is even open, and how far away it sounds.
// Nothing in sim/ includes anything from audio/, and a build with sound
// compiled out would still produce the same events and simply drop them.
//
// The queue is an out-parameter of the step functions rather than a member of
// SimState on purpose. SimState is copied every step and interpolated between
// two copies; an event is neither copyable state nor interpolatable - it
// happened once, at one step, and is consumed once. Putting it in SimState
// would have meant deciding what interpolate() does with half an explosion.

enum class Cue : uint8_t {
    LaserFire,
    VulcanFire,
    ConcussionFire,
    HomingFire,
    RailgunFire,
    LightningFire,      // one crackle per fireInterval while the trigger is held
    FlareThrow,
    WeaponEmpty,        // trigger pulled with no ammo or no energy
    WeaponSelect,
    WallImpact,         // a bolt that hit geometry
    Explosion,          // anything with a splash radius going off
    RobotHit,           // a bolt landing on a robot that survived it
    RobotFire,
    RobotAlert,         // a robot noticing the player
    RobotDeath,
    ObjectDestroyed,    // destructible scenery, including the reactor
    PlayerHurt,
    PlayerDeath,
    DoorOpen,
    DoorClose,
    DoorLocked,         // the key for this door is not held
    Pickup,
    KeyPickup,
    ReactorDestroyed,
    CountdownWarn,      // one per second while the mine comes apart
    Exit,
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Cue::Count)> kCueNames{
    "laser_fire",  "vulcan_fire",  "concussion_fire", "homing_fire",      "railgun_fire",
    "lightning_fire", "flare_throw", "weapon_empty",  "weapon_select",    "wall_impact",
    "explosion",   "robot_hit",    "robot_fire",      "robot_alert",      "robot_death",
    "object_destroyed", "player_hurt", "player_death", "door_open",       "door_close",
    "door_locked", "pickup",       "key_pickup",      "reactor_destroyed", "countdown_warn",
    "exit",
};

// A sound that wants playing. `segment` is carried because the emitter knows it
// for free and a listener that needed to attenuate through the mine would want
// it; the mixer currently uses distance alone.
struct SoundEvent {
    Cue cue = Cue::LaserFire;
    glm::vec3 position{0.0f};
    int segment = 0;
    float gain = 1.0f;
    // Sounds the player makes - their own guns, their own hull being hit - play
    // at the listener rather than at a point in the world. A gun firing from
    // the ship's right wingtip is half a metre from the camera, and panning it
    // by that offset puts the player's own laser hard right, which is wrong and
    // very obviously wrong.
    bool atListener = false;
};

// Sized for the worst step rather than the average: a concussion missile going
// off in a room full of robots can register a dozen events in one 8 ms step.
// A full queue drops the newest event, matching what the projectile and effect
// pools do when they fill.
inline constexpr int kMaxSoundEvents = 64;

struct EventQueue {
    std::array<SoundEvent, kMaxSoundEvents> items{};
    int count = 0;
    // Counted, not silently discarded: an overflow that happens every step is
    // a capacity that is wrong, and --sound-test reports this number.
    int dropped = 0;

    void push(Cue cue, const glm::vec3& position, int segment, float gain = 1.0f,
              bool atListener = false) {
        if (count >= kMaxSoundEvents) {
            ++dropped;
            return;
        }
        SoundEvent& e = items[static_cast<size_t>(count++)];
        e.cue = cue;
        e.position = position;
        e.segment = segment;
        e.gain = gain;
        e.atListener = atListener;
    }

    // At the listener: the player's own guns and their own hull.
    void pushLocal(Cue cue, float gain = 1.0f) {
        push(cue, glm::vec3(0.0f), 0, gain, /*atListener=*/true);
    }

    void clear() { count = 0; }
};

// Every step function takes one of these and it is always allowed to be null:
// the headless tests in game/ drive the simulation without any interest in
// sound, and a null queue is how they say so without threading a dummy through.
inline void emit(EventQueue* q, Cue cue, const glm::vec3& position, int segment,
                 float gain = 1.0f) {
    if (q) q->push(cue, position, segment, gain);
}

inline void emitLocal(EventQueue* q, Cue cue, float gain = 1.0f) {
    if (q) q->pushLocal(cue, gain);
}

} // namespace dvk::sim
