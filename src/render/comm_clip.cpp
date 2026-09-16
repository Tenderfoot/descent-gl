#include "render/comm_clip.h"

#include "core/alloc_count.h"
#include "core/log.h"

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>

namespace dvk::gl {

namespace {

struct ClipTiming {
    float framesPerSecond = 0.0f;
    float displaySeconds = 0.0f;
};

nlohmann::json readManifest(const std::filesystem::path& manifestPath) {
    std::ifstream in(manifestPath);
    if (!in) log::fatal("comm manifest '{}' would not open", manifestPath.string());
    nlohmann::json manifest;
    try {
        in >> manifest;
    } catch (const std::exception& e) {
        log::fatal("comm manifest '{}' is invalid: {}", manifestPath.string(), e.what());
    }
    return manifest;
}

// The file a manifest names to be decoded, or empty for an authored sequence.
std::string readVideoName(const std::filesystem::path& manifestPath) {
    const nlohmann::json manifest = readManifest(manifestPath);
    if (!manifest.contains("video")) return {};
    if (!manifest["video"].is_string()) {
        log::fatal("comm manifest '{}' has a non-string 'video'", manifestPath.string());
    }
    return manifest["video"].get<std::string>();
}

ClipTiming readTiming(const std::filesystem::path& manifestPath) {
    const nlohmann::json manifest = readManifest(manifestPath);
    ClipTiming timing;
    timing.framesPerSecond = manifest.value("framesPerSecond", 0.0f);
    timing.displaySeconds = manifest.value("displaySeconds", 0.0f);
    return timing;
}

} // namespace

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
    // Every clip shares one texture, so the first one to load fixes the size
    // and the rest have to agree with it - whichever kind they are.
    const auto requireSize = [&](const std::filesystem::path& manifest, int width, int height) {
        if (library.clips.empty()) {
            library.width = width;
            library.height = height;
        } else if (width != library.width || height != library.height) {
            log::fatal("comm clip '{}' is {}x{}; library requires {}x{}", manifest.string(),
                       width, height, library.width, library.height);
        }
    };

    for (const std::filesystem::path& manifest : manifests) {
        CommClipInfo info;
        info.name = manifest.parent_path().filename().string();
        if (library.find(info.name) >= 0) log::fatal("duplicate comm clip '{}'", info.name);

        // A manifest naming a video file is decoded; one listing frames is
        // uploaded. Both kinds coexist so a library can be converted a clip at
        // a time rather than all at once.
        const std::string video = readVideoName(manifest);
        if (!video.empty()) {
            CommVideo decoder;
            if (!decoder.open(manifest.parent_path() / video)) {
                log::fatal("comm clip '{}' names video '{}' that will not play", info.name,
                           video);
            }
            requireSize(manifest, decoder.width(), decoder.height());

            const ClipTiming timing = readTiming(manifest);
            info.framesPerSecond = timing.framesPerSecond > 0.0f ? timing.framesPerSecond
                                                                 : decoder.framesPerSecond();
            // The stream's own length is the default, so a manifest does not
            // have to restate what the file already knows.
            info.displaySeconds =
                timing.displaySeconds > 0.0f
                    ? timing.displaySeconds
                    : static_cast<float>(decoder.frameCount()) / decoder.framesPerSecond();
            info.frameCount = decoder.frameCount();
            // One scratch layer, rewritten as the picture changes.
            info.baseLayer = static_cast<int>(library.frames.size());
            info.video = static_cast<int>(library.videos.size());
            library.frames.emplace_back(static_cast<size_t>(library.width) *
                                            static_cast<size_t>(library.height) * 4,
                                        0u);
            library.videos.push_back(std::move(decoder));
            library.clips.push_back(std::move(info));
            continue;
        }

        CommClip clip = loadCommClip(manifest);
        requireSize(manifest, clip.width, clip.height);
        info.baseLayer = static_cast<int>(library.frames.size());
        info.frameCount = static_cast<int>(clip.frames.size());
        info.framesPerSecond = clip.framesPerSecond;
        info.displaySeconds = clip.displaySeconds;
        library.clips.push_back(std::move(info));
        library.frames.insert(library.frames.end(),
                              std::make_move_iterator(clip.frames.begin()),
                              std::make_move_iterator(clip.frames.end()));
    }
    log::info("comm library '{}': {} clip(s) ({} decoded), {} texture layer(s)", root.string(),
              library.clips.size(), library.videos.size(), library.frames.size());
    return library;
}

namespace {

// Cheap content hash, so "is this actually a different picture" and "is this
// the same picture as last time" are answerable without keeping copies.
uint64_t hashPixels(const uint8_t* rgba, size_t bytes) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < bytes; ++i) {
        h ^= rgba[i];
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace

int runCommTest(const std::filesystem::path& root) {
    CommClipLibrary library = loadCommClips(root);
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

    // --- decoded clips ---------------------------------------------------
    //
    // The timing above is arithmetic and holds for either kind of clip. This is
    // the part that only a decoder can get wrong.
    const size_t frameBytes = expected;
    for (const CommClipInfo& clip : library.clips) {
        if (clip.video < 0) continue;
        CommVideo& video = library.videos[static_cast<size_t>(clip.video)];

        const uint8_t* first = video.frame(0);
        // Already decoded at open, so asking for it again is the no-change
        // case and must say so rather than re-decoding it.
        if (first != nullptr) {
            log::error("comm-test: clip '{}' re-decoded a frame it was already showing",
                       clip.name);
            ok = false;
        }

        const int decodesBefore = video.decodeCount();
        const int seeksBefore = video.seekCount();
        uint64_t hashes[4] = {0, 0, 0, 0};
        for (int i = 1; i <= 3; ++i) {
            const uint8_t* pixels = video.frame(i);
            if (!pixels) {
                log::error("comm-test: clip '{}' produced no frame {}", clip.name, i);
                ok = false;
                break;
            }
            hashes[i] = hashPixels(pixels, frameBytes);
            // Every pixel must be opaque. plm_frame_to_rgba writes three bytes
            // per pixel and leaves the fourth alone, so a decoder that does not
            // set alpha itself produces a panel that is entirely transparent -
            // which on screen is a backing rectangle with nothing in it, and
            // reads as a broken decode rather than as a missing byte.
            for (size_t b = 3; b < frameBytes; b += 4) {
                if (pixels[b] != 0xFFu) {
                    log::error("comm-test: clip '{}' frame {} is not opaque at byte {}",
                               clip.name, i, b);
                    ok = false;
                    break;
                }
            }
        }
        // Playing forward must take the cheap path. A clip that seeks its way
        // through sequential playback works and is doing it the expensive way,
        // which is exactly the kind of thing that only shows up as a frame-time
        // spike much later.
        if (video.seekCount() != seeksBefore) {
            log::error("comm-test: clip '{}' seeked {} time(s) playing three frames forward",
                       clip.name, video.seekCount() - seeksBefore);
            ok = false;
        }
        if (video.decodeCount() != decodesBefore + 3) {
            log::error("comm-test: clip '{}' decoded {} frames to advance three", clip.name,
                       video.decodeCount() - decodesBefore);
            ok = false;
        }
        if (hashes[1] == hashes[2] || hashes[2] == hashes[3]) {
            log::error("comm-test: clip '{}' returned the same picture for consecutive frames",
                       clip.name);
            ok = false;
        }

        // Seeking back has to land on the same pixels, or --comm would put a
        // different frame in the screenshot every run and the flag would stop
        // meaning anything.
        const uint8_t* again = video.frame(1);
        if (!again || hashPixels(again, frameBytes) != hashes[1]) {
            log::error("comm-test: clip '{}' did not reproduce frame 1 after seeking back",
                       clip.name);
            ok = false;
        }

        // A message can hold the panel up for longer than the video lasts, so
        // running off the end has to wrap rather than stop. Deliberately lands
        // on a frame other than the one showing: an index that wraps onto the
        // current frame is the no-change case and correctly returns nothing,
        // which would pass this for the wrong reason.
        const uint8_t* wrapped = video.frame(clip.frameCount + 2);
        if (!wrapped) {
            log::error("comm-test: clip '{}' produced nothing past its last frame", clip.name);
            ok = false;
        } else if (hashPixels(wrapped, frameBytes) != hashes[2]) {
            log::error("comm-test: clip '{}' did not wrap to frame 2 past its end", clip.name);
            ok = false;
        }
        // The decode happens on the draw path, which CLAUDE.md says does not
        // reach the heap. pl_mpeg takes its buffers at open and the RGBA target
        // is allocated once beside them, so steady-state playback should ask
        // for nothing - measured here rather than assumed, the way --sound-test
        // measures the mixer.
        video.frame(0);
        for (int i = 1; i <= 8; ++i) video.frame(i);   // warm up past the seek
        dvk::alloc::Scope allocations;
        for (int i = 9; i < 120; ++i) video.frame(i);
        const uint64_t allocated = allocations.count();

        log::info("  clip '{}': {} frames decoded, {} seeks, one texture layer, "
                  "{} allocation(s) over 111 decoded frames",
                  clip.name, video.decodeCount(), video.seekCount(), allocated);
        if (allocated > 0) {
            log::error("comm-test: clip '{}' allocated {} time(s) ({} bytes) while decoding",
                       clip.name, allocated, allocations.bytes());
            ok = false;
        }
    }

    if (!ok) {
        log::error("comm-test: failed shared dimensions, layers, playback timing or decoding");
        return 1;
    }
    log::info("comm-test: {} clip(s), {} consistent {}x{} frames, deterministic timing and loop: passed",
              library.clips.size(), library.frames.size(), library.width, library.height);
    return 0;
}

} // namespace dvk::gl
