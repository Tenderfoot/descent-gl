#pragma once

#include "audio/wav.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <glm/vec3.hpp>
#include <vector>

namespace dvk::audio {

// One output rate for the whole system. Clips are resampled to it on load so a
// voice never carries a rate conversion the mixer would have to get right, and
// so a 22 kHz file and a 48 kHz file cost exactly the same to play.
inline constexpr int kSampleRate = 44100;
inline constexpr int kChannels = 2;

// The ceiling on simultaneous sounds. Reached in practice only by sustained
// vulcan fire in a room of robots, which is also the one case where nobody can
// tell which individual shot went missing.
inline constexpr int kMaxVoices = 32;

// Continuous sounds that are not one-shots: they start, follow something, and
// stop. Kept as a fixed set of named channels rather than as loop handles,
// because these channels have fixed roles and lifetimes.
enum class Ambient : uint8_t {
    Engine,     // the ship's own drive, gained by throttle
    Alarm,      // the countdown klaxon
    // A transmission's voice. Unlike the two above it carries a different clip
    // each time and does not loop, so it has no default file in the bank - the
    // caller names the recording. What it wants from this list is the one thing
    // every entry gets: a burst of gunfire cannot steal it mid-sentence.
    Comm,
    MusicLull,
    MusicCombat,
    Count,
};

struct Listener {
    glm::vec3 position{0.0f};
    // The ship's own basis. `right` is what the pan is computed against, and
    // `forward` is kept so a front/back cue could be added without changing
    // the command protocol.
    glm::vec3 right{1.0f, 0.0f, 0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
};

// How sound falls off with distance. Deliberately a struct and not constants:
// --sound-test drives the attenuation curve directly, and a test that had to
// recompile to try a different rolloff would not get run.
struct Attenuation {
    // Inside this, a sound is at full volume. About half a segment.
    float reference = 9.0f;
    // Beyond this it is not mixed at all. Roughly ten segments, which is
    // further than the portal walk will ever have drawn.
    float maximum = 220.0f;
    // How fast it falls between the two. 1 is inverse-distance.
    float rolloff = 1.0f;
};

// What the game thread asks the audio thread to do.
//
// One command type for everything, pushed through one queue, rather than the
// game thread reaching into the voice pool behind a lock. The audio callback
// runs on a thread owned by the driver and must never block: if it waits on a
// mutex the game thread happens to hold, the result is a buffer underrun,
// which is an audible click. Commands make that structurally impossible -
// the game thread only ever writes the queue's head, the audio thread only
// ever writes its tail, and neither waits for the other.
struct Command {
    enum class Kind : uint8_t { Play, SetListener, SetAmbient } kind = Kind::Play;

    int clip = -1;               // index into the bank; -1 stops an ambient
    glm::vec3 position{0.0f};
    float gain = 1.0f;
    float pitch = 1.0f;
    bool positional = true;
    uint8_t ambient = 0;         // which Ambient channel, for SetAmbient
    // Whether a SetAmbient voice repeats when it runs out. Ignored by Play,
    // which is a one-shot by definition. The engine and the klaxon both want
    // this; a channel taken for something that ends on its own does not, and
    // asks for the channel's protection from being stolen without its loop.
    bool loop = true;
    // Music transports keep advancing at zero gain so a crossfade never
    // restarts the track that was inaudible. Ordinary ambients retain the
    // existing zero-means-stop behaviour.
    bool keepAliveAtZero = false;
    // Zero uses the short anti-zipper glide. Music supplies an intentional
    // transition time for combat crossfades and transmission ducking.
    float gainGlideSeconds = 0.0f;
    Listener listener;
};

// A single-producer, single-consumer ring. `head` is written only by the
// producer and `tail` only by the consumer, which is what makes the two
// relaxed/acquire-release pairs below sufficient without a lock.
template <class T, int N>
struct Ring {
    std::array<T, N> items{};
    std::atomic<uint32_t> head{0};
    std::atomic<uint32_t> tail{0};

    bool push(const T& value) {
        const uint32_t h = head.load(std::memory_order_relaxed);
        const uint32_t next = (h + 1) % static_cast<uint32_t>(N);
        // Full. The newest request is dropped, the same way the projectile and
        // effect pools drop theirs - a queue this deep only fills if the audio
        // thread has stopped, and then nothing would be heard anyway.
        if (next == tail.load(std::memory_order_acquire)) return false;
        items[h] = value;
        head.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        const uint32_t t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire)) return false;
        out = items[t];
        tail.store((t + 1) % static_cast<uint32_t>(N), std::memory_order_release);
        return true;
    }
};

inline constexpr int kCommandQueueSize = 256;

struct Voice {
    const Clip* clip = nullptr;
    double cursor = 0.0;          // position in source frames
    float pitch = 1.0f;
    float gain = 1.0f;
    glm::vec3 position{0.0f};
    bool positional = true;
    bool loop = false;
    int ambient = -1;             // the channel that owns it, or -1 for a one-shot
    float gainGlideSeconds = 0.0f;
    // Smoothed, because a voice whose panning jumps between blocks zippers
    // audibly. -1 marks "not yet computed", so the first block of a voice
    // starts at its correct gain rather than sliding up from silence.
    float gainL = -1.0f;
    float gainR = -1.0f;
    bool active = false;
};

// The whole audio system minus the device: takes commands in, writes
// interleaved stereo float out. Knows nothing about miniaudio, about GLFW, or
// about the game - which is what lets --sound-test run every line of it with no
// sound card and no window.
class Mixer {
public:
    // `clips` must outlive the mixer. It is the bank, indexed by the `clip`
    // field of a Play command.
    void setClips(const std::vector<Clip>* clips) { clips_ = clips; }

    // Called from the game thread. Never blocks.
    bool push(const Command& command) { return commands_.push(command); }

    // Called from the audio thread. Writes `frames * kChannels` floats.
    // Allocates nothing: the voice pool and the queue are fixed arrays, which
    // --sound-test checks with the same allocation counter main.cpp uses on the
    // sim step.
    void render(float* out, int frames);

    // Master volume, 0..1. Atomic because it is the one thing the game thread
    // changes that is not worth a command.
    void setMasterGain(float gain) { masterGain_.store(gain, std::memory_order_relaxed); }
    float masterGain() const { return masterGain_.load(std::memory_order_relaxed); }

    // Sound effect volume, 0..1: every voice except the music and a
    // transmission's voice. Applied under the master, and before the per-voice
    // glide, so moving it does not step.
    void setEffectsGain(float gain) { effectsGain_.store(gain, std::memory_order_relaxed); }
    float effectsGain() const { return effectsGain_.load(std::memory_order_relaxed); }

    Attenuation attenuation;

    // Diagnostics, for --sound-test and the debug panel.
    int activeVoices() const;
    uint64_t started() const { return started_; }
    uint64_t stolen() const { return stolen_; }
    const Listener& listener() const { return listener_; }

private:
    void drain();
    void start(const Command& command);
    // Which slot a new voice should take: a free one, else the quietest active
    // one that is not a protected ambient channel. Dropping the newest sound is
    // worse than cutting the least audible one, so this only returns -1 when
    // every slot is a protected channel and there is nothing left to cut.
    int claimSlot() const;
    void spatialise(const Voice& voice, float& left, float& right) const;

    const std::vector<Clip>* clips_ = nullptr;
    std::array<Voice, kMaxVoices> voices_{};
    // Which slot each ambient channel is using, so a second SetAmbient updates
    // the existing voice instead of layering another copy on top of it.
    std::array<int, static_cast<size_t>(Ambient::Count)> ambientSlots_ = [] {
        std::array<int, static_cast<size_t>(Ambient::Count)> slots{};
        slots.fill(-1);
        return slots;
    }();
    Ring<Command, kCommandQueueSize> commands_;
    Listener listener_;
    std::atomic<float> masterGain_{0.8f};
    std::atomic<float> effectsGain_{1.0f};
    uint64_t started_ = 0;
    uint64_t stolen_ = 0;
};

} // namespace dvk::audio
