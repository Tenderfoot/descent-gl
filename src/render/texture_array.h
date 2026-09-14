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
    TextureArray(int size, const std::vector<std::vector<uint8_t>>& layers, const char* label);
    ~TextureArray();
    TextureArray(const TextureArray&) = delete;
    TextureArray& operator=(const TextureArray&) = delete;

    void bind(GLuint unit) const { glBindTextureUnit(unit, id_); }
    int layerCount() const { return layers_; }

private:
    GLuint id_ = 0;
    int layers_ = 0;
};

} // namespace dvk::gl
