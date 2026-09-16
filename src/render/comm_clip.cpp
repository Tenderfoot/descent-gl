#include "render/comm_clip.h"

#include "core/log.h"

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>

namespace dvk::gl {

int CommClip::frameAt(float seconds) const {
    if (frames.empty() || framesPerSecond <= 0.0f) return 0;
    const int raw = static_cast<int>(std::floor(std::max(0.0f, seconds) * framesPerSecond));
    return raw % static_cast<int>(frames.size());
}

int CommClipInfo::frameAt(float seconds) const {
    if (frameCount <= 0 || framesPerSecond <= 0.0f) return baseLayer;
    const int raw = static_cast<int>(std::floor(std::max(0.0f, seconds) * framesPerSecond));
    return baseLayer + raw % frameCount;
}

int CommClipLibrary::find(std::string_view name) const {
    for (size_t i = 0; i < clips.size(); ++i) {
        if (clips[i].name == name) return static_cast<int>(i);
    }
    return -1;
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

CommClipLibrary loadCommClips(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> manifests;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(root)) {
        const std::filesystem::path manifest = entry.path() / "comm.json";
        if (entry.is_directory() && std::filesystem::is_regular_file(manifest)) {
            manifests.push_back(manifest);
        }
    }
    std::sort(manifests.begin(), manifests.end());
    if (manifests.empty()) log::fatal("comm root '{}' contains no clips", root.string());

    CommClipLibrary library;
    for (const std::filesystem::path& manifest : manifests) {
        CommClip clip = loadCommClip(manifest);
        if (library.clips.empty()) {
            library.width = clip.width;
            library.height = clip.height;
        } else if (clip.width != library.width || clip.height != library.height) {
            log::fatal("comm clip '{}' is {}x{}; library requires {}x{}", manifest.string(),
                       clip.width, clip.height, library.width, library.height);
        }

        CommClipInfo info;
        info.name = manifest.parent_path().filename().string();
        info.baseLayer = static_cast<int>(library.frames.size());
        info.frameCount = static_cast<int>(clip.frames.size());
        info.framesPerSecond = clip.framesPerSecond;
        info.displaySeconds = clip.displaySeconds;
        if (library.find(info.name) >= 0) log::fatal("duplicate comm clip '{}'", info.name);
        library.clips.push_back(std::move(info));
        library.frames.insert(library.frames.end(),
                              std::make_move_iterator(clip.frames.begin()),
                              std::make_move_iterator(clip.frames.end()));
    }
    log::info("comm library '{}': {} clip(s), {} total frame(s)", root.string(),
              library.clips.size(), library.frames.size());
    return library;
}

int runCommTest(const std::filesystem::path& root) {
    const CommClipLibrary library = loadCommClips(root);
    bool ok = library.width > 0 && library.height > 0 && !library.frames.empty();
    const size_t expected = static_cast<size_t>(library.width) *
                            static_cast<size_t>(library.height) * 4;
    for (const auto& frame : library.frames) ok &= frame.size() == expected;
    for (const CommClipInfo& clip : library.clips) {
        ok &= clip.frameCount > 0;
        ok &= clip.frameAt(0.0f) == clip.baseLayer;
        if (clip.frameCount > 1) {
            ok &= clip.frameAt(1.0f / clip.framesPerSecond) == clip.baseLayer + 1;
            ok &= clip.frameAt(static_cast<float>(clip.frameCount) / clip.framesPerSecond) ==
                  clip.baseLayer;
        }
    }

    if (!ok) {
        log::error("comm-test: failed shared dimensions, layers, or playback timing");
        return 1;
    }
    log::info("comm-test: {} clip(s), {} consistent {}x{} frames, deterministic timing and loop: passed",
              library.clips.size(), library.frames.size(), library.width, library.height);
    return 0;
}

} // namespace dvk::gl
