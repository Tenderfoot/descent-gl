#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
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

struct CommClipInfo {
    std::string name;
    int baseLayer = 0;
    int frameCount = 0;
    float framesPerSecond = 0.0f;
    float displaySeconds = 0.0f;

    int frameAt(float seconds) const;
};

struct CommClipLibrary {
    int width = 0;
    int height = 0;
    std::vector<CommClipInfo> clips;
    std::vector<std::vector<uint8_t>> frames;

    int find(std::string_view name) const;
};

CommClip loadCommClip(const std::filesystem::path& manifestPath);
CommClipLibrary loadCommClips(const std::filesystem::path& root);
int runCommTest(const std::filesystem::path& root);

} // namespace dvk::gl
