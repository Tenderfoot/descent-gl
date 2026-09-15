#include "audio/audio.h"

#include "audio/device.h"
#include "core/log.h"

#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat3x3.hpp>

namespace dvk::audio {
namespace {

using sim::Cue;

// The mix, as a table. Gains here are relative to each other, not absolute:
// the master gain sets the level, and these say that a laser is quieter than a
// missile and that a door is quieter than either.
//
// Several cues share a file on purpose. A robot dying and a crate being
// destroyed are both something coming apart, and giving them separate files
// would mean two nearly identical recordings to keep in step.
constexpr std::array<CueDef, static_cast<size_t>(Cue::Count)> kCues{{
    {"laser.wav", 0.55f, 0.06f},          // LaserFire
    {"vulcan.wav", 0.45f, 0.11f},         // VulcanFire
    {"concussion.wav", 0.85f, 0.04f},     // ConcussionFire
    {"homing.wav", 0.85f, 0.04f},         // HomingFire
    {"railgun.wav", 0.90f, 0.03f},        // RailgunFire
    {"lightning.wav", 0.55f, 0.10f},      // LightningFire
    {"flare.wav", 0.40f, 0.05f},          // FlareThrow
    {"empty.wav", 0.35f, 0.02f},          // WeaponEmpty
    {"select.wav", 0.30f, 0.00f},         // WeaponSelect
    {"impact.wav", 0.45f, 0.14f},         // WallImpact
    {"explosion.wav", 0.95f, 0.09f},      // Explosion
    {"robot_hit.wav", 0.50f, 0.12f},      // RobotHit
    {"robot_fire.wav", 0.55f, 0.09f},     // RobotFire
    {"robot_alert.wav", 0.50f, 0.07f},    // RobotAlert
    {"explosion.wav", 0.90f, 0.10f},      // RobotDeath
    {"explosion.wav", 0.80f, 0.08f},      // ObjectDestroyed
    {"player_hurt.wav", 0.70f, 0.05f},    // PlayerHurt
    {"player_death.wav", 1.00f, 0.00f},   // PlayerDeath
    {"door_open.wav", 0.55f, 0.03f},      // DoorOpen
    {"door_close.wav", 0.55f, 0.03f},     // DoorClose
    {"locked.wav", 0.55f, 0.02f},         // DoorLocked
    {"pickup.wav", 0.55f, 0.04f},         // Pickup
    {"key_pickup.wav", 0.70f, 0.00f},     // KeyPickup
    {"reactor.wav", 1.00f, 0.00f},        // ReactorDestroyed
    {"countdown.wav", 0.60f, 0.00f},      // CountdownWarn
    {"exit.wav", 0.90f, 0.00f},           // Exit
}};

constexpr std::array<const char*, static_cast<size_t>(Ambient::Count)> kAmbientFiles{{
    "engine.wav",   // Engine
    "alarm.wav",    // Alarm
}};

} // namespace

const std::array<CueDef, static_cast<size_t>(Cue::Count)>& cueTable() {
    return kCues;
}

void Bank::load(const std::string& dir) {
    clips.clear();
    cueClip.fill(-1);
    ambientClip.fill(-1);
    missing = 0;

    // Decode each distinct filename once, then point every cue that named it at
    // the same clip.
    std::vector<std::string> loaded;
    const auto clipFor = [&](const char* file) -> int {
        if (!file || !*file) return -1;
        for (size_t i = 0; i < loaded.size(); ++i) {
            if (loaded[i] == file) return static_cast<int>(i);
        }
        Clip clip = loadWav(dir + "/" + file);
        if (!clip.valid()) {
            ++missing;
            return -1;
        }
        resample(clip, kSampleRate);
        loaded.emplace_back(file);
        clips.push_back(std::move(clip));
        return static_cast<int>(clips.size() - 1);
    };

    for (size_t i = 0; i < kCues.size(); ++i) {
        cueClip[i] = clipFor(kCues[i].file);
    }
    for (size_t i = 0; i < kAmbientFiles.size(); ++i) {
        ambientClip[i] = clipFor(kAmbientFiles[i]);
    }
}

Audio::Audio() = default;

Audio::~Audio() {
    shutdown();
}

bool Audio::init(const std::string& soundDir, bool openDevice) {
    bank_.load(soundDir);
    mixer_.setClips(&bank_.clips);

    int silent = 0;
    for (int index : bank_.cueClip) silent += index < 0 ? 1 : 0;
    if (silent > 0) {
        log::warn("sound: {} of {} cues have no clip and will be silent", silent,
                  static_cast<int>(bank_.cueClip.size()));
    }

    if (!openDevice) return false;

    device_ = std::make_unique<Device>();
    deviceOpen_ = device_->open(mixer_);
    if (!deviceOpen_) {
        // A container with no sound card is the ordinary case here, not a
        // failure worth stopping for. The mixer still runs; nothing pulls from
        // it, and the game is silent.
        log::warn("sound: no playback device, running silently");
        device_.reset();
        return false;
    }
    log::info("sound: {} clips, {} voices, {} Hz on \"{}\"", bank_.clips.size(), kMaxVoices,
              kSampleRate, device_->name());
    return true;
}

void Audio::shutdown() {
    if (device_) {
        device_->close();
        device_.reset();
    }
    deviceOpen_ = false;
}

void Audio::setListener(const glm::vec3& position, const glm::quat& orientation) {
    const glm::mat3 basis = glm::mat3_cast(orientation);
    Command command;
    command.kind = Command::Kind::SetListener;
    command.listener.position = position;
    command.listener.right = basis * glm::vec3(1.0f, 0.0f, 0.0f);
    command.listener.forward = basis * glm::vec3(0.0f, 0.0f, -1.0f);
    mixer_.push(command);
}

float Audio::jitter(float half) {
    if (half <= 0.0f) return 1.0f;
    // xorshift, local to the audio system. Nothing here feeds back into the
    // simulation, so this deliberately does not share the sim's generator -
    // drawing from that would make a replay depend on how many sounds played.
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    const float unit = static_cast<float>(rng_ & 0xFFFFFFu) / 16777215.0f;   // 0..1
    return 1.0f + (unit * 2.0f - 1.0f) * half;
}

void Audio::play(sim::Cue cue, const glm::vec3& position, float gain) {
    const auto index = static_cast<size_t>(cue);
    if (index >= bank_.cueClip.size()) return;
    const int clip = bank_.cueClip[index];
    if (clip < 0) return;

    Command command;
    command.kind = Command::Kind::Play;
    command.clip = clip;
    command.position = position;
    command.gain = kCues[index].gain * gain;
    command.pitch = jitter(kCues[index].pitchJitter);
    command.positional = true;
    mixer_.push(command);
}

void Audio::playLocal(sim::Cue cue, float gain) {
    const auto index = static_cast<size_t>(cue);
    if (index >= bank_.cueClip.size()) return;
    const int clip = bank_.cueClip[index];
    if (clip < 0) return;

    Command command;
    command.kind = Command::Kind::Play;
    command.clip = clip;
    command.gain = kCues[index].gain * gain;
    command.pitch = jitter(kCues[index].pitchJitter);
    command.positional = false;
    mixer_.push(command);
}

void Audio::submit(const sim::EventQueue& events) {
    for (int i = 0; i < events.count; ++i) {
        const sim::SoundEvent& e = events.items[static_cast<size_t>(i)];
        const auto index = static_cast<size_t>(e.cue);
        if (index < played_.size()) ++played_[index];
        if (e.atListener) {
            playLocal(e.cue, e.gain);
        } else {
            play(e.cue, e.position, e.gain);
        }
    }
    // Assigned, not accumulated: EventQueue::dropped is a running total for the
    // life of the queue, and clear() deliberately does not reset it.
    dropped_ = static_cast<uint32_t>(events.dropped);
}

uint32_t Audio::totalPlayed() const {
    uint32_t total = 0;
    for (uint32_t n : played_) total += n;
    return total;
}

void Audio::setAmbient(Ambient channel, float gain, float pitch) {
    const auto index = static_cast<size_t>(channel);
    if (index >= bank_.ambientClip.size()) return;

    Command command;
    command.kind = Command::Kind::SetAmbient;
    command.ambient = static_cast<uint8_t>(index);
    command.clip = gain > 0.0f ? bank_.ambientClip[index] : -1;
    command.gain = gain;
    command.pitch = pitch;
    // Ambient channels ride with the listener: the engine is the player's own
    // ship and the klaxon is the whole mine.
    command.positional = false;
    mixer_.push(command);
}

void Audio::setMuted(bool muted) {
    if (muted == muted_) return;
    muted_ = muted;
    if (muted) {
        unmutedGain_ = mixer_.masterGain();
        mixer_.setMasterGain(0.0f);
    } else {
        mixer_.setMasterGain(unmutedGain_);
    }
}

void Audio::setMasterGain(float gain) {
    unmutedGain_ = gain;
    mixer_.setMasterGain(muted_ ? 0.0f : gain);
}

} // namespace dvk::audio
