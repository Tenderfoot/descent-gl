#pragma once

#include <string>
#include <vector>

namespace dvk::audio {

// A decoded sound: mono or stereo 32-bit float, interleaved when stereo, at
// whatever rate the file carried. Positional sounds are authored mono; stereo
// is retained for non-positional material such as music.
struct Clip {
    std::vector<float> samples;
    int sampleRate = 0;
    int channels = 1;
    std::string name;

    bool valid() const {
        return !samples.empty() && sampleRate > 0 && (channels == 1 || channels == 2) &&
               samples.size() % static_cast<size_t>(channels) == 0;
    }
    size_t frames() const { return samples.size() / static_cast<size_t>(channels); }
    float seconds() const {
        return sampleRate > 0 ? static_cast<float>(frames()) / static_cast<float>(sampleRate) : 0.0f;
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

// Reads an Ogg Vorbis file into the same shape loadWav produces. The music is
// Vorbis so the repository carries a tenth of the bytes; once decoded it costs
// exactly what the WAV did. Lives in ogg.cpp.
Clip loadOgg(const std::string& path);

// loadOgg for a `.ogg` path, loadWav for anything else.
Clip loadClip(const std::string& path);

// Resamples in place to `rate` if the clip is at some other rate, so the mixer
// only ever deals in one sample rate and does not carry a per-voice ratio it
// would have to get right. Linear interpolation: these are short, noisy sound
// effects and the artefacts of a better kernel would not be audible over them.
void resample(Clip& clip, int rate);

} // namespace dvk::audio
