#pragma once

#include "audio/mixer.h"
#include "audio/wav.h"
#include "sim/events.h"

#include <array>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>
#include <string_view>
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
    std::array<int, 2> musicClip{{-1, -1}};   // lull, combat
    // What those two tracks are called, in the same order, for the HUD to
    // show. The filename with its extension taken off: the files are the
    // authoring, so naming them anywhere else would be a second place to keep
    // in step. Empty for a track that did not load, which is how a caller
    // tells the difference between silence and a nameless track.
    std::array<std::string, 2> musicNames{};
    int missing = 0;

    // A transmission's voice track, keyed by the comm clip it belongs to. Named
    // rather than enumerated because which transmissions exist is content: a
    // level's Lua names one, and the directory under assets/comm either has a
    // comm.wav beside its pictures or it does not.
    std::vector<std::string> commNames;
    std::vector<int> commClips;

    // -1 for a transmission with no voice track, which is not an error - the
    // panel plays silently.
    int commClip(std::string_view name) const;

    // Scans `root` (assets/comm) for <clip>/comm.wav and appends what it finds.
    // Call after load(): it adds to the same bank, so a voice track costs the
    // mixer nothing it does not already do.
    void loadComm(const std::string& root);

    // Loads the two long-form stereo tracks from assets/music. They join the
    // same immutable bank as effects and comms before the mixer sees it.
    void loadMusic(const std::string& root);

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
    //
    // `commDir` is assets/comm, scanned for transmission voice tracks; empty
    // skips that and every transmission plays silently. `musicDir` contains
    // the fixed lull and combat tracks; empty disables adaptive music.
    bool init(const std::string& soundDir, bool openDevice = true,
              const std::string& commDir = "", const std::string& musicDir = "");
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

    // A transmission's voice. `clip` indexes the bank (Bank::commClip), and -1
    // or a gain of zero stops whatever is playing. It rides the Comm ambient
    // channel, so it survives a firefight, and it does not loop: it ends with
    // its own recording.
    //
    // Nothing here tries to hold it in step with the picture beyond starting
    // them together. The video runs on the application clock and this runs on
    // the device's, and over a transmission's length the two drift by less than
    // either is worth correcting for. See docs/ARCHITECTURE.md.
    void playComm(int clip, float gain = 1.0f);
    void stopComm();

    // Both music transports run together. `combat` is the state the music is
    // heading for: 0 for the lull and 1 for combat, with intermediate values
    // crossfaded at equal power. It is a target rather than a level, because
    // this holds the envelope that walks towards it - two seconds into combat
    // and two seconds back out, so neither track arrives or departs on a
    // single frame. `dt` advances that envelope; pass zero to freeze it, which
    // is what a paused game does. A comm ducks the shared music bus on its own
    // envelope, over the top of the crossfade and without touching effects or
    // the voice itself.
    void setMusicState(float combat, float volume, bool commDucked, float dt);
    void stopMusic();

    // Where the crossfade actually is, rather than where the game last asked
    // it to go: 0 is the lull alone, 1 is combat alone, and anything between
    // is both tracks audible at once. The walk towards the target is clamped
    // rather than exponential, so the ends are reached exactly and a caller
    // can compare against them without an epsilon.
    float musicMix() const { return musicCombatMix_; }
    // Whether there is a soundtrack running at all. False before the first
    // setMusicState, after stopMusic, and when neither track loaded - which
    // includes a build with no assets/music, where the transports would
    // otherwise be running on nothing.
    bool musicPlaying() const {
        return musicRunning_ && (bank_.musicClip[0] >= 0 || bank_.musicClip[1] >= 0);
    }
    // The lull track's name, then the combat one's. Empty for a track that did
    // not load. Stable for the lifetime of the bank, so a per-frame caller can
    // hold the pointer rather than copying the string.
    const std::string& musicName(int index) const {
        static const std::string none;
        return index == 0 || index == 1 ? bank_.musicNames[static_cast<size_t>(index)] : none;
    }

    void setMasterGain(float gain);
    float masterGain() const { return muted_ ? unmutedGain_ : mixer_.masterGain(); }

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
    bool musicRunning_ = false;
    // Where the crossfade and the duck actually are, as opposed to where the
    // game last asked them to be. Both are walked towards their target a
    // frame at a time; holding them here is what makes a transition a
    // duration rather than a step.
    float musicCombatMix_ = 0.0f;
    float musicDuck_ = 1.0f;
    uint32_t rng_ = 0x9e3779b9u;
    std::array<uint32_t, static_cast<size_t>(sim::Cue::Count)> played_{};
    uint32_t dropped_ = 0;

    float jitter(float half);
};

} // namespace dvk::audio
