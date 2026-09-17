#include "render/sprites.h"

#include "core/log.h"

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <utility>

namespace dvk::gl {
namespace {

// Area-average `src` (srcSize square, RGBA) down to `dstSize` square.
//
// The sprites are authored far larger than they are ever drawn - a pickup is a
// hundred pixels on screen at most - so they are resampled once at load rather
// than costing twenty times the VRAM and the mip chain to match. A box filter
// over the exact source rectangle each destination pixel covers is enough for
// art that is already soft; the ratio is not an integer, so the rectangle
// boundaries are computed per pixel rather than stepped.
std::vector<uint8_t> downsample(const uint8_t* src, int srcSize, int dstSize) {
    std::vector<uint8_t> dst(static_cast<size_t>(dstSize) * static_cast<size_t>(dstSize) * 4);
    for (int y = 0; y < dstSize; ++y) {
        const int y0 = y * srcSize / dstSize;
        const int y1 = std::max(y0 + 1, (y + 1) * srcSize / dstSize);
        for (int x = 0; x < dstSize; ++x) {
            const int x0 = x * srcSize / dstSize;
            const int x1 = std::max(x0 + 1, (x + 1) * srcSize / dstSize);
            std::array<uint32_t, 4> sum{0, 0, 0, 0};
            for (int sy = y0; sy < y1; ++sy) {
                const uint8_t* row = src + (static_cast<size_t>(sy) * srcSize + x0) * 4;
                for (int sx = x0; sx < x1; ++sx, row += 4) {
                    sum[0] += row[0];
                    sum[1] += row[1];
                    sum[2] += row[2];
                    sum[3] += row[3];
                }
            }
            const uint32_t n = static_cast<uint32_t>(x1 - x0) * static_cast<uint32_t>(y1 - y0);
            uint8_t* out = dst.data() + (static_cast<size_t>(y) * dstSize + x) * 4;
            for (size_t c = 0; c < 4; ++c) out[c] = static_cast<uint8_t>(sum[c] / n);
        }
    }
    return dst;
}

// Carries the colour of the opaque pixels outward into the transparent ones,
// a ring at a time, leaving the alpha channel alone.
//
// The fringe this avoids is the one loadPremultiplied() avoids by
// premultiplying, arriving by another road. An exported wordmark is
// transparent *black* outside its letters, and every filtered sample near an
// edge - bilinear minification and every level of the mip chain - averages
// that black in and rings the letters in grey. Premultiplying is not the fix
// here, because the HUD blends straight alpha rather than additively, so
// instead the colour under the transparent pixels stops being black. Nothing
// reads it where alpha is zero; it only has to be the wrong thing to average.
//
// Four rings is enough for the mip levels a logo drawn at half its size is
// actually sampled from.
void bleedEdges(std::vector<uint8_t>& rgba, int w, int h, int rings = 4) {
    std::vector<uint8_t> known(static_cast<size_t>(w) * static_cast<size_t>(h));
    for (size_t i = 0; i < known.size(); ++i) known[i] = rgba[i * 4 + 3] > 0 ? 1 : 0;

    std::vector<size_t> filled;
    for (int ring = 0; ring < rings; ++ring) {
        filled.clear();
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t index = static_cast<size_t>(y) * w + x;
                if (known[index]) continue;
                std::array<uint32_t, 3> sum{0, 0, 0};
                uint32_t n = 0;
                for (const auto& [dx, dy] : std::array<std::pair<int, int>, 4>{
                         {{-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const size_t neighbour = static_cast<size_t>(ny) * w + nx;
                    if (!known[neighbour]) continue;
                    for (size_t c = 0; c < 3; ++c) sum[c] += rgba[neighbour * 4 + c];
                    ++n;
                }
                if (n == 0) continue;
                for (size_t c = 0; c < 3; ++c) {
                    rgba[index * 4 + c] = static_cast<uint8_t>(sum[c] / n);
                }
                // Marked after the ring finishes, so a colour cannot travel
                // several pixels in one pass and smear along a row.
                filled.push_back(index);
            }
        }
        if (filled.empty()) break;
        for (size_t index : filled) known[index] = 1;
    }
}

// One RGBA image, premultiplied and resampled to `size`.
//
// Premultiplied because both of the things that happen to these pixels next
// need it. Averaging straight alpha - in the box filter below and again in the
// mip chain the GPU builds - pulls the transparent background's black into
// every edge pixel and leaves a dark fringe around the sprite. And the pass
// that draws them is additive (GL_ONE, GL_ONE), which ignores the alpha channel
// entirely, so the fade at a sprite's edge has to be carried by the colour.
std::vector<uint8_t> loadPremultiplied(const char* path, int size) {
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load(path, &w, &h, &channels, 4);
    if (!pixels) {
        const char* why = stbi_failure_reason();
        log::fatal("sprite '{}' would not load: {}", path, why ? why : "no reason given");
    }
    // Both are fatal rather than corrected: a texture array layer is a fixed
    // square, and silently stretching or upscaling art to fit it hides the
    // mistake in the asset until someone notices the sprite looks wrong.
    if (w != h) log::fatal("sprite '{}' is {}x{}; sprites must be square", path, w, h);
    if (w < size) {
        log::fatal("sprite '{}' is {}px, smaller than the {}px it is drawn at", path, w, size);
    }

    const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t a = pixels[i * 4 + 3];
        for (size_t c = 0; c < 3; ++c) {
            pixels[i * 4 + c] = static_cast<uint8_t>(pixels[i * 4 + c] * a / 255);
        }
    }

    std::vector<uint8_t> out = w == size ? std::vector<uint8_t>(pixels, pixels + count * 4)
                                         : downsample(pixels, w, size);
    stbi_image_free(pixels);
    log::info("sprite '{}' {}x{} -> {}x{}", path, w, h, size, size);
    return out;
}

}   // namespace

std::vector<std::vector<uint8_t>> loadPickupSprites(int size) {
    // A PNG's first row is its top; a GL texture's first row is the bottom, and
    // the billboard quad puts v = 0 at the bottom corner. The mirror of the
    // stbi_flip_vertically_on_write() screenshot.cpp needs for the same reason.
    // Set here rather than per image because nothing else in the project reads
    // an image file, so there is no other caller to surprise.
    stbi_set_flip_vertically_on_load(1);

    // Indexed by PickupSprite, so the order here is the layer order.
    static constexpr std::array<const char*, PickupSpriteCount> kPaths{
        "assets/images/health.png", "assets/images/energy.png", "assets/images/key.png"};

    std::vector<std::vector<uint8_t>> layers;
    layers.reserve(kPaths.size());
    for (const char* path : kPaths) layers.push_back(loadPremultiplied(path, size));
    return layers;
}

LoadedImage loadMenuLogo(const char* path) {
    // HUD UVs put v = 0 at the top of a quad, matching a PNG's first row, and
    // loadPickupSprites() leaves stb_image's process-global flip set for its
    // world-space sprites. Set explicitly rather than inherited, the same way
    // comm_clip.cpp does.
    stbi_set_flip_vertically_on_load(0);

    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load(path, &w, &h, &channels, 4);
    if (!pixels) {
        const char* why = stbi_failure_reason();
        log::fatal("logo '{}' would not load: {}", path, why ? why : "no reason given");
    }

    // The opaque bounds. Anything outside them is export padding.
    int minX = w, minY = h, maxX = -1, maxY = -1;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (pixels[(static_cast<size_t>(y) * w + x) * 4 + 3] == 0) continue;
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
    }
    if (maxX < minX || maxY < minY) {
        stbi_image_free(pixels);
        log::fatal("logo '{}' is entirely transparent", path);
    }

    LoadedImage image;
    image.width = maxX - minX + 1;
    image.height = maxY - minY + 1;
    image.rgba.resize(static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4);
    for (int y = 0; y < image.height; ++y) {
        const uint8_t* src = pixels + (static_cast<size_t>(y + minY) * w + minX) * 4;
        uint8_t* dst = image.rgba.data() + static_cast<size_t>(y) * image.width * 4;
        std::copy(src, src + static_cast<size_t>(image.width) * 4, dst);
    }
    stbi_image_free(pixels);

    bleedEdges(image.rgba, image.width, image.height);

    log::info("logo '{}' {}x{} -> {}x{} after trim", path, w, h, image.width, image.height);
    return image;
}

}   // namespace dvk::gl
