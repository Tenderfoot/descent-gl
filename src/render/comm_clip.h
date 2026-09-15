#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace dvk::gl {

// CPU-side authored animation. Frames are decoded once at startup and then
// uploaded as layers of one texture array; playback itself only selects a
// layer and never decodes or allocates.
struct CommClip {
    int width = 0;
    int height = 0;
    float framesPerSecond = 0.0f;
    float displaySeconds = 0.0f;
    std::vector<std::vector<uint8_t>> frames;

    int frameAt(float seconds) const;
};

CommClip loadCommClip(const std::filesystem::path& manifestPath);
int runCommTest(const std::filesystem::path& manifestPath);

} // namespace dvk::gl
