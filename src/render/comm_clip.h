#pragma once

#include "render/comm_video.h"

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
    // Where this clip's pictures live in the library's one texture array. A
    // frame sequence owns `frameCount` consecutive layers from here. A decoded
    // clip owns exactly one, rewritten as its picture changes, which is what
    // makes its cost independent of how long it runs.
    int baseLayer = 0;
    int frameCount = 0;
    float framesPerSecond = 0.0f;
    float displaySeconds = 0.0f;
    // Index into CommClipLibrary::videos, or -1 for an authored sequence.
    int video = -1;

    int frameAt(float seconds) const;
};

// Every clip the game can play, sharing one texture. Move-only, because a
// decoder is not something to copy.
struct CommClipLibrary {
    int width = 0;
    int height = 0;
    std::vector<CommClipInfo> clips;
    // Uploaded once at startup: every authored frame, plus one blank scratch
    // layer for each decoded clip.
    std::vector<std::vector<uint8_t>> frames;
    std::vector<CommVideo> videos;

    int find(std::string_view name) const;
};

CommClip loadCommClip(const std::filesystem::path& manifestPath);
CommClipLibrary loadCommClips(const std::filesystem::path& root);
int runCommTest(const std::filesystem::path& root);

} // namespace dvk::gl
