#pragma once

#include <string>
#include <vector>

namespace dvk::audio {

// A decoded sound: mono 32-bit float, at whatever rate the file carried.
//
// Mono because every sound in this game is a point in a mine and gets its
// stereo image from where that point is relative to the ship. A stereo source
// has no position to pan, so a stereo file is downmixed on load rather than
// being quietly half-played.
struct Clip {
    std::vector<float> samples;
    int sampleRate = 0;
    std::string name;

    bool valid() const { return !samples.empty() && sampleRate > 0; }
    float seconds() const {
        return sampleRate > 0 ? static_cast<float>(samples.size()) / static_cast<float>(sampleRate)
                              : 0.0f;
    }
};

// Reads a RIFF/WAVE file: 8/16/24/32-bit PCM or 32/64-bit float, any channel
// count, any rate. Returns a clip with `valid() == false` and logs why on any
// failure, so a missing or malformed sound is a warning and a silent cue rather
// than a crash.
//
// This is a hand-written parser rather than miniaudio's decoder, which would do
// the same job. The reason is containment: keeping miniaudio to the one file
// that opens the device means the bank, the mixer and everything --sound-test
// exercises can be built and run with no audio library at all. It is about a
// hundred lines to avoid linking a dependency into the part of the system that
// most needs to be testable.
Clip loadWav(const std::string& path);

// Resamples in place to `rate` if the clip is at some other rate, so the mixer
// only ever deals in one sample rate and does not carry a per-voice ratio it
// would have to get right. Linear interpolation: these are short, noisy sound
// effects and the artefacts of a better kernel would not be audible over them.
void resample(Clip& clip, int rate);

} // namespace dvk::audio
