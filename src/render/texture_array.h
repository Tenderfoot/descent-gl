#pragma once

#include "render/gl.h"

#include <cstdint>
#include <vector>

namespace dvk::gl {

// A 2D texture array of same-sized tiles. A wall's texture index selects the
// layer, so the whole mine draws with one binding and the per-segment draw
// calls never have to be split by material.
class TextureArray {
public:
    // Tiles repeat across a surface; sprites are a picture with an edge, and
    // wrapping one samples the far side of it along that edge.
    enum class Wrap { Repeat, ClampToEdge };

    TextureArray(int size, const std::vector<std::vector<uint8_t>>& layers, const char* label,
                 Wrap wrap = Wrap::Repeat);
    TextureArray(int width, int height, const std::vector<std::vector<uint8_t>>& layers,
                 const char* label, Wrap wrap = Wrap::Repeat);
    ~TextureArray();
    TextureArray(const TextureArray&) = delete;
    TextureArray& operator=(const TextureArray&) = delete;

    void bind(GLuint unit) const { glBindTextureUnit(unit, id_); }
    int layerCount() const { return layers_; }

    // Rewrites one layer's pixels in place. For a decoded comm clip, whose
    // single layer changes as its picture does while every layer around it
    // stays put.
    //
    // This regenerates the whole array's mipmaps, because GL has no per-layer
    // form of it and the HUD samples this array minified. That cost is
    // proportional to the array's total layers rather than to the one that
    // changed, and it is paid when a video frame changes - about 24 times a
    // second, not once a draw. Worth revisiting if a library ever holds enough
    // authored sequences for the sweep to matter; with a handful of clips it
    // does not.
    void updateLayer(int layer, const uint8_t* rgba) const;

private:
    GLuint id_ = 0;
    int layers_ = 0;
    int width_ = 0;
    int height_ = 0;
};

} // namespace dvk::gl
