#include "render/comm_video.h"

#include "core/log.h"

// The one place pl_mpeg exists. See the note in comm_video.h.
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

#include <cmath>
#include <vector>

namespace dvk::gl {

struct CommVideo::Impl {
    plm_t* plm = nullptr;
    // Allocated once at open and reused for every frame, because the draw path
    // this feeds is not allowed to reach the heap.
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    float framesPerSecond = 0.0f;
    int frameCount = 0;
    int current = -1;
    int decodes = 0;
    int seeks = 0;

    ~Impl() {
        if (plm) plm_destroy(plm);
    }

    bool store(plm_frame_t* frame, int index) {
        if (!frame) return false;
        plm_frame_to_rgba(frame, rgba.data(), width * 4);
        current = index;
        return true;
    }
};

CommVideo::CommVideo() : impl_(std::make_unique<Impl>()) {}
CommVideo::~CommVideo() = default;
CommVideo::CommVideo(CommVideo&&) noexcept = default;
CommVideo& CommVideo::operator=(CommVideo&&) noexcept = default;

bool CommVideo::open(const std::filesystem::path& file) {
    Impl& s = *impl_;
    s.plm = plm_create_with_filename(file.string().c_str());
    if (!s.plm) {
        log::error("comm video '{}' would not open", file.string());
        return false;
    }
    if (plm_get_num_video_streams(s.plm) <= 0) {
        log::error("comm video '{}' carries no video stream", file.string());
        return false;
    }
    // The game's own mixer plays a transmission's sound. Decoding an audio
    // track we would then throw away costs time on the draw path for nothing,
    // and the authoring tool does not write one.
    plm_set_audio_enabled(s.plm, 0);
    plm_set_loop(s.plm, 0);

    s.width = plm_get_width(s.plm);
    s.height = plm_get_height(s.plm);
    s.framesPerSecond = static_cast<float>(plm_get_framerate(s.plm));
    const double duration = plm_get_duration(s.plm);
    if (s.width <= 0 || s.height <= 0 || s.framesPerSecond <= 0.0f || duration <= 0.0) {
        log::error("comm video '{}' reports {}x{} at {} fps over {}s, which is not playable",
                   file.string(), s.width, s.height, s.framesPerSecond, duration);
        return false;
    }
    // Floor, not round: a count that overshoots by one leaves a frame index
    // that decodes to nothing and reads as the end of the clip.
    s.frameCount = std::max(1, static_cast<int>(std::floor(duration *
                                                           static_cast<double>(
                                                               s.framesPerSecond))));
    // Filled with 0xFF, not zero, and that is load-bearing: plm_frame_to_rgba
    // writes the three colour bytes of each pixel and leaves the fourth exactly
    // as it found it. Starting from zero gives a fully transparent panel that
    // draws its backing rectangle and nothing else - which looks like a decode
    // failure and is not one. Alpha is written once here and never again.
    s.rgba.assign(static_cast<size_t>(s.width) * static_cast<size_t>(s.height) * 4, 0xFFu);

    if (!s.store(plm_decode_video(s.plm), 0)) {
        log::error("comm video '{}' decoded no first frame", file.string());
        return false;
    }
    ++s.decodes;
    log::info("comm video '{}' {}x{}, {} frames at {:.1f} fps, {:.2f}s", file.string(), s.width,
              s.height, s.frameCount, s.framesPerSecond, duration);
    return true;
}

bool CommVideo::valid() const { return impl_->plm != nullptr && impl_->frameCount > 0; }
int CommVideo::width() const { return impl_->width; }
int CommVideo::height() const { return impl_->height; }
float CommVideo::framesPerSecond() const { return impl_->framesPerSecond; }
int CommVideo::frameCount() const { return impl_->frameCount; }
int CommVideo::decodeCount() const { return impl_->decodes; }
int CommVideo::seekCount() const { return impl_->seeks; }

const uint8_t* CommVideo::frame(int index) {
    Impl& s = *impl_;
    if (!valid()) return nullptr;
    // The caller counts within the clip and may run past it, because a message
    // can hold the panel up for longer than the video lasts. Wrapping here is
    // what the frame-sequence path gets from its modulo.
    index = index % s.frameCount;
    if (index < 0) index += s.frameCount;
    if (index == s.current) return nullptr;

    // Sequential is the whole point: a P-frame is expressed against the one
    // before it, so playing forward is the cheap path and seeking is not.
    if (index == s.current + 1) {
        if (s.store(plm_decode_video(s.plm), index)) {
            ++s.decodes;
            return s.rgba.data();
        }
    }
    // Anywhere else - a wrap back to the start, a re-triggered message, --comm
    // pinning a fixed point - costs a seek. Exact, so the frame is the one
    // asked for, which decodes forward from the preceding intra frame; the
    // authoring tool keeps the GOP short so that stays cheap.
    plm_frame_t* sought = plm_seek_frame(
        s.plm, static_cast<double>(index) / static_cast<double>(s.framesPerSecond), TRUE);
    ++s.seeks;
    if (s.store(sought, index)) return s.rgba.data();

    // Only if the file is truncated or lied about its duration. Hold whatever
    // was last decoded rather than drawing a torn or empty panel.
    log::error("comm video could not produce frame {} of {}", index, s.frameCount);
    return nullptr;
}

} // namespace dvk::gl
