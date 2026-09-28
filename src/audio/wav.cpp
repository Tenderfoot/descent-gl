#include "audio/wav.h"

#include "core/log.h"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace dvk::audio {
namespace {

uint32_t readU32(const uint8_t* p) {
    // Spelled out rather than memcpy'd into a uint32_t: WAV is little-endian on
    // every machine, and the host might not be.
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t readU16(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

// One sample of `bits` at `p`, as a float in -1..1.
float sampleAt(const uint8_t* p, int bits, bool isFloat) {
    if (isFloat) {
        if (bits == 32) {
            float v = 0.0f;
            std::memcpy(&v, p, 4);
            return v;
        }
        double v = 0.0;
        std::memcpy(&v, p, 8);
        return static_cast<float>(v);
    }
    switch (bits) {
        case 8:
            // 8-bit PCM is the odd one out: unsigned, centred on 128.
            return (static_cast<float>(p[0]) - 128.0f) / 128.0f;
        case 16: {
            const auto v = static_cast<int16_t>(readU16(p));
            return static_cast<float>(v) / 32768.0f;
        }
        case 24: {
            int32_t v = static_cast<int32_t>(static_cast<uint32_t>(p[0]) |
                                             (static_cast<uint32_t>(p[1]) << 8) |
                                             (static_cast<uint32_t>(p[2]) << 16));
            if (v & 0x800000) v |= ~0xFFFFFF;   // sign-extend the 24th bit
            return static_cast<float>(v) / 8388608.0f;
        }
        case 32: {
            const auto v = static_cast<int32_t>(readU32(p));
            return static_cast<float>(v) / 2147483648.0f;
        }
        default:
            return 0.0f;
    }
}

} // namespace

Clip loadWav(const std::string& path) {
    Clip clip;
    clip.name = path;

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        log::warn("sound: cannot open {}", path);
        return clip;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
    if (bytes.size() < 44) {
        log::warn("sound: {} is too short to be a WAV ({} bytes)", path, bytes.size());
        return clip;
    }
    if (std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        log::warn("sound: {} is not a RIFF/WAVE file", path);
        return clip;
    }

    // Walk the chunk list rather than assuming `fmt ` then `data` at fixed
    // offsets. Plenty of tools write a LIST or a fact chunk in between, and a
    // fixed-offset reader turns that into noise rather than into an error.
    int channels = 0;
    int bits = 0;
    bool isFloat = false;
    const uint8_t* data = nullptr;
    size_t dataSize = 0;

    size_t at = 12;
    while (at + 8 <= bytes.size()) {
        const uint8_t* header = bytes.data() + at;
        const uint32_t size = readU32(header + 4);
        const size_t body = at + 8;
        if (body + size > bytes.size()) {
            // A truncated final chunk: take what is actually there. Some
            // encoders write the size they intended rather than the size they
            // wrote, and the audio before the cut is still good.
            if (std::memcmp(header, "data", 4) == 0 && body < bytes.size()) {
                data = bytes.data() + body;
                dataSize = bytes.size() - body;
            }
            break;
        }
        if (std::memcmp(header, "fmt ", 4) == 0 && size >= 16) {
            const uint16_t format = readU16(bytes.data() + body);
            channels = readU16(bytes.data() + body + 2);
            clip.sampleRate = static_cast<int>(readU32(bytes.data() + body + 4));
            bits = readU16(bytes.data() + body + 14);
            // 1 is PCM, 3 is IEEE float. 0xFFFE is WAVE_FORMAT_EXTENSIBLE,
            // whose real format sits in a GUID at the end of the chunk; the
            // first two bytes of that GUID are the format tag.
            if (format == 0xFFFE && size >= 40) {
                const uint16_t sub = readU16(bytes.data() + body + 24);
                isFloat = sub == 3;
            } else {
                isFloat = format == 3;
            }
            if (format != 1 && format != 3 && format != 0xFFFE) {
                log::warn("sound: {} is compressed (format {}), which this loader does not read",
                          path, format);
                return clip;
            }
        } else if (std::memcmp(header, "data", 4) == 0) {
            data = bytes.data() + body;
            dataSize = size;
        }
        // Chunks are word-aligned: an odd size is followed by a pad byte.
        at = body + size + (size & 1u);
    }

    if (!data || channels <= 0 || bits <= 0 || clip.sampleRate <= 0) {
        log::warn("sound: {} has no usable fmt/data chunk pair", path);
        return clip;
    }

    const int bytesPerSample = bits / 8;
    const size_t frameBytes = static_cast<size_t>(bytesPerSample) * static_cast<size_t>(channels);
    if (frameBytes == 0) {
        log::warn("sound: {} declares {} bits and {} channels", path, bits, channels);
        return clip;
    }
    const size_t frames = dataSize / frameBytes;
    // Music is stereo. Positional effects are mono, and unusual multichannel
    // sources keep the old safe behaviour of being averaged to one channel.
    clip.channels = channels == 2 ? 2 : 1;
    clip.samples.resize(frames * static_cast<size_t>(clip.channels));
    for (size_t f = 0; f < frames; ++f) {
        if (clip.channels == 2) {
            for (int c = 0; c < 2; ++c) {
                clip.samples[f * 2 + static_cast<size_t>(c)] =
                    sampleAt(data + (f * frameBytes) +
                                         static_cast<size_t>(c) *
                                             static_cast<size_t>(bytesPerSample),
                             bits, isFloat);
            }
            continue;
        }
        // Downmix by averaging. A sum would clip a stereo file that was already
        // near full scale on both sides.
        float sum = 0.0f;
        for (int c = 0; c < channels; ++c) {
            sum += sampleAt(data + (f * frameBytes) + (static_cast<size_t>(c) *
                                                       static_cast<size_t>(bytesPerSample)),
                            bits, isFloat);
        }
        clip.samples[f] = sum / static_cast<float>(channels);
    }
    return clip;
}

Clip loadClip(const std::string& path) {
    std::string extension = std::filesystem::path(path).extension().string();
    for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return extension == ".ogg" ? loadOgg(path) : loadWav(path);
}

void resample(Clip& clip, int rate) {
    if (!clip.valid() || rate <= 0 || clip.sampleRate == rate) return;

    const double ratio = static_cast<double>(clip.sampleRate) / static_cast<double>(rate);
    const auto inFrames = clip.frames();
    const auto outFrames = static_cast<size_t>(static_cast<double>(inFrames) / ratio);
    if (outFrames == 0) {
        clip.samples.clear();
        return;
    }
    std::vector<float> out(outFrames * static_cast<size_t>(clip.channels));
    for (size_t i = 0; i < outFrames; ++i) {
        const double src = static_cast<double>(i) * ratio;
        const auto i0 = static_cast<size_t>(src);
        const size_t i1 = std::min(i0 + 1, inFrames - 1);
        const auto t = static_cast<float>(src - static_cast<double>(i0));
        for (int c = 0; c < clip.channels; ++c) {
            const size_t outIndex = i * static_cast<size_t>(clip.channels) + static_cast<size_t>(c);
            const size_t a = i0 * static_cast<size_t>(clip.channels) + static_cast<size_t>(c);
            const size_t b = i1 * static_cast<size_t>(clip.channels) + static_cast<size_t>(c);
            out[outIndex] = clip.samples[a] * (1.0f - t) + clip.samples[b] * t;
        }
    }
    clip.samples.swap(out);
    clip.sampleRate = rate;
}

} // namespace dvk::audio
