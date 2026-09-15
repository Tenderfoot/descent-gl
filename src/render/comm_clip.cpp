#include "render/comm_clip.h"

#include "core/log.h"

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>

namespace dvk::gl {

int CommClip::frameAt(float seconds) const {
    if (frames.empty() || framesPerSecond <= 0.0f) return 0;
    const int raw = static_cast<int>(std::floor(std::max(0.0f, seconds) * framesPerSecond));
    return raw % static_cast<int>(frames.size());
}

CommClip loadCommClip(const std::filesystem::path& manifestPath) {
    std::ifstream in(manifestPath);
    if (!in) log::fatal("comm manifest '{}' would not open", manifestPath.string());

    nlohmann::json manifest;
    try {
        in >> manifest;
    } catch (const std::exception& e) {
        log::fatal("comm manifest '{}' is invalid: {}", manifestPath.string(), e.what());
    }

    CommClip clip;
    clip.framesPerSecond = manifest.value("framesPerSecond", 0.0f);
    clip.displaySeconds = manifest.value("displaySeconds", 0.0f);
    if (clip.framesPerSecond <= 0.0f || clip.displaySeconds <= 0.0f) {
        log::fatal("comm manifest '{}' needs positive framesPerSecond and displaySeconds",
                   manifestPath.string());
    }
    if (!manifest.contains("frames") || !manifest["frames"].is_array() ||
        manifest["frames"].empty()) {
        log::fatal("comm manifest '{}' has no frames", manifestPath.string());
    }

    // HUD UVs put v=0 at the top of the panel, matching a PNG's first row.
    // Pickup loading deliberately flips its world-space sprites, so set this
    // explicitly rather than inheriting stb_image's process-global setting.
    stbi_set_flip_vertically_on_load(0);
    const std::filesystem::path directory = manifestPath.parent_path();
    clip.frames.reserve(manifest["frames"].size());
    for (const nlohmann::json& entry : manifest["frames"]) {
        if (!entry.is_string()) {
            log::fatal("comm manifest '{}' contains a non-string frame", manifestPath.string());
        }
        const std::filesystem::path path = directory / entry.get<std::string>();
        int width = 0, height = 0, channels = 0;
        stbi_uc* pixels = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
        if (!pixels) {
            const char* why = stbi_failure_reason();
            log::fatal("comm frame '{}' would not load: {}", path.string(),
                       why ? why : "no reason given");
        }
        if (clip.frames.empty()) {
            clip.width = width;
            clip.height = height;
        } else if (width != clip.width || height != clip.height) {
            stbi_image_free(pixels);
            log::fatal("comm frame '{}' is {}x{}; expected {}x{}", path.string(), width, height,
                       clip.width, clip.height);
        }
        const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
        clip.frames.emplace_back(pixels, pixels + bytes);
        stbi_image_free(pixels);
    }

    log::info("comm clip '{}' {}x{}, {} frames at {:.1f} fps, {:.1f}s display",
              manifestPath.string(), clip.width, clip.height, clip.frames.size(),
              clip.framesPerSecond, clip.displaySeconds);
    return clip;
}

int runCommTest(const std::filesystem::path& manifestPath) {
    const CommClip clip = loadCommClip(manifestPath);
    bool ok = clip.width > 0 && clip.height > 0 && !clip.frames.empty();
    ok &= clip.frameAt(0.0f) == 0;
    if (clip.frames.size() > 1) {
        ok &= clip.frameAt(1.0f / clip.framesPerSecond) == 1;
        ok &= clip.frameAt(static_cast<float>(clip.frames.size()) / clip.framesPerSecond) == 0;
    }
    const size_t expected = static_cast<size_t>(clip.width) * static_cast<size_t>(clip.height) * 4;
    for (const auto& frame : clip.frames) ok &= frame.size() == expected;

    if (!ok) {
        log::error("comm-test: failed frame dimensions or playback timing");
        return 1;
    }
    log::info("comm-test: {} consistent {}x{} frames, deterministic timing and loop: passed",
              clip.frames.size(), clip.width, clip.height);
    return 0;
}

} // namespace dvk::gl
