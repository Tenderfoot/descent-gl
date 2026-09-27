// Ogg Vorbis, for the soundtrack. stb_vorbis is part of the stb package that
// is already on the allowlist; it gets a translation unit of its own because
// its implementation defines single-letter macros that nothing else should
// have to live alongside.
#include "audio/wav.h"

#include "core/log.h"

#include <cstdlib>

#include <stb_vorbis.c>

namespace dvk::audio {

Clip loadOgg(const std::string& path) {
    Clip clip;
    clip.name = path;

    int channels = 0;
    int rate = 0;
    short* decoded = nullptr;
    const int frames = stb_vorbis_decode_filename(path.c_str(), &channels, &rate, &decoded);
    if (frames <= 0 || !decoded || channels <= 0 || rate <= 0) {
        log::warn("sound: cannot decode {} as Ogg Vorbis", path);
        std::free(decoded);
        return clip;
    }

    // The same channel rule as loadWav: stereo stays stereo, anything else is
    // averaged down to mono.
    const auto count = static_cast<size_t>(frames);
    const auto stride = static_cast<size_t>(channels);
    clip.sampleRate = rate;
    clip.channels = channels == 2 ? 2 : 1;
    clip.samples.resize(count * static_cast<size_t>(clip.channels));
    if (clip.channels == 2) {
        for (size_t i = 0; i < count * 2; ++i) {
            clip.samples[i] = static_cast<float>(decoded[i]) / 32768.0f;
        }
    } else {
        for (size_t f = 0; f < count; ++f) {
            float sum = 0.0f;
            for (size_t c = 0; c < stride; ++c) {
                sum += static_cast<float>(decoded[f * stride + c]) / 32768.0f;
            }
            clip.samples[f] = sum / static_cast<float>(channels);
        }
    }
    std::free(decoded);
    return clip;
}

} // namespace dvk::audio
