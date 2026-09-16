#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

namespace dvk::gl {

// One MPEG-1 comm clip, decoded a frame at a time.
//
// `comm_video.cpp` is the only file that includes pl_mpeg, and the only one
// that defines PL_MPEG_IMPLEMENTATION - the same containment `device.cpp` gives
// miniaudio and `render/gl.h` gives glad. Nothing else in the project knows a
// decoder exists; the renderer is handed RGBA pixels.
//
// Why a decoder rather than the authored frame sequences beside it: a texture
// array costs width*height*4 bytes for every frame of every clip, resident from
// startup, so a library's video memory and the repository both grow with the
// total authored seconds across all of it. A 20-second clip at 24 fps is 480
// frames - about 126 MB of texture with mipmaps, and tens of megabytes of PNGs
// that re-encode in full every time anyone re-authors them. Decoded, the same
// clip is one reused texture and a file of a few hundred KiB, and a longer one
// costs no more.
class CommVideo {
public:
    CommVideo();
    ~CommVideo();
    CommVideo(CommVideo&&) noexcept;
    CommVideo& operator=(CommVideo&&) noexcept;
    CommVideo(const CommVideo&) = delete;
    CommVideo& operator=(const CommVideo&) = delete;

    // Opens the file and decodes its first frame, so a clip that cannot be
    // played fails at load rather than in the middle of one. Returns false and
    // logs why; the caller decides whether that is fatal.
    bool open(const std::filesystem::path& file);

    bool valid() const;
    int width() const;
    int height() const;
    float framesPerSecond() const;
    int frameCount() const;

    // RGBA for `index`, decoding forward when it is the next one and seeking
    // when it is not. Returns null when `index` is already what was last
    // returned, so the caller can skip the upload - which is the common case,
    // a 24 fps clip drawn on a 120 Hz loop changing picture once in five.
    // Allocates nothing after open().
    const uint8_t* frame(int index);

    // What the clip did, for --comm-test: a run that seeks once per frame is
    // working but doing it the expensive way.
    int decodeCount() const;
    int seekCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dvk::gl
