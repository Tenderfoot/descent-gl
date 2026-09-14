#pragma once

#include "audio/mixer.h"
#include "audio/wav.h"
#include "sim/events.h"

#include <array>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>
#include <vector>

namespace dvk::audio {

class Device;

// How one cue is played. Data, so retuning the mix means editing a table rather
// than hunting for a magic number at a call site.
struct CueDef {
    const char* file = "";
    float gain = 1.0f;
    // Half-width of the random detune, as a fraction of the sample rate. A
    // burst of vulcan fire is the same 40 ms of audio a dozen times over, and
    // without this it reads as a machine skipping rather than as a gun.
    float pitchJitter = 0.0f;
};

// Every clip on disk, plus which one each cue and each ambient channel uses.
// Files are decoded once and shared: several cues deliberately name the same
// file, and the reactor going up should not cost a second copy of the explosion.
struct Bank {
    std::vector<Clip> clips;
    std::array<int, static_cast<size_t>(sim::Cue::Count)> cueClip{};
    std::array<int, static_cast<size_t>(Ambient::Count)> ambientClip{};
    int missing = 0;

    // Loads from `dir` (assets/sounds). Missing or malformed files are warned
    // about and left as a silent cue; sound is never a reason the game will not
    // start.
    void load(const std::string& dir);
};

const std::array<CueDef, static_cast<size_t>(sim::Cue::Count)>& cueTable();

// The game's view of sound: hand it the simulation's events and where the ship
// is, and it does the rest.
//
// `src/audio` is to sound what `src/render` is to graphics, and the same rule
// applies in reverse: nothing under `src/sim` includes this header. The
// simulation reports events (sim/events.h) and this is the only thing that
// turns them into noise.
class Audio {
public:
    Audio();
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    // Loads the bank and, unless `openDevice` is false, starts a playback
    // device. Returns false only if the device could not be opened - a headless
    // container with no sound card is the normal case for that, and it is a
    // warning, not an error: the game runs silently and everything else is
    // unaffected. --sound-test passes false and exercises the whole mixer with
    // no device at all.
    bool init(const std::string& soundDir, bool openDevice = true);
    void shutdown();

    bool deviceOpen() const { return deviceOpen_; }
    Mixer& mixer() { return mixer_; }
    const Mixer& mixer() const { return mixer_; }
    const Bank& bank() const { return bank_; }

    // Where the player is hearing from. Called once per frame, before submit.
    void setListener(const glm::vec3& position, const glm::quat& orientation);

    // Plays everything the last simulation step reported. Safe to call with an
    // empty queue, and does nothing at all if no clip loaded for a cue.
    void submit(const sim::EventQueue& events);

    // One-off, for sounds the app layer owns rather than the simulation:
    // toggling the headlight, opening the map.
    void play(sim::Cue cue, const glm::vec3& position, float gain = 1.0f);
    void playLocal(sim::Cue cue, float gain = 1.0f);

    // The continuous channels. `gain` of zero stops one. `pitch` is how the
    // engine is made to rise with the throttle.
    void setAmbient(Ambient channel, float gain, float pitch = 1.0f);

    void setMasterGain(float gain) { mixer_.setMasterGain(gain); }
    float masterGain() const { return mixer_.masterGain(); }

    // Muting keeps the device open and the ambient channels running, so
    // unmuting is instant and does not restart the engine loop mid-note.
    void setMuted(bool muted);
    bool muted() const { return muted_; }

    // How many of each cue have been played since start-up, and how many the
    // simulation had to drop because its queue was full.
    //
    // Nothing reads these to make a decision; they exist so a headless run can
    // report what it heard. Sound is the one subsystem a screenshot cannot
    // show, and without a count, "the reactor makes a noise" is a claim about
    // code rather than an observation about a run.
    const std::array<uint32_t, static_cast<size_t>(sim::Cue::Count)>& played() const {
        return played_;
    }
    uint32_t totalPlayed() const;
    uint32_t droppedEvents() const { return dropped_; }

private:
    Mixer mixer_;
    Bank bank_;
    // A pointer rather than a member so that miniaudio.h is included by exactly
    // one translation unit. Including a four-megabyte header everywhere that
    // wants to make a noise is how a build gets slow.
    std::unique_ptr<Device> device_;
    bool deviceOpen_ = false;
    bool muted_ = false;
    float unmutedGain_ = 0.8f;
    uint32_t rng_ = 0x9e3779b9u;
    std::array<uint32_t, static_cast<size_t>(sim::Cue::Count)> played_{};
    uint32_t dropped_ = 0;

    float jitter(float half);
};

} // namespace dvk::audio
