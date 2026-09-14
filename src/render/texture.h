#pragma once

#include "render/gl.h"

#include <string>

namespace dvk::gl {

// A 2D texture loaded from disk with stb_image. Mipmapped, repeat wrap,
// anisotropic if the driver offers it.
class Texture2D {
public:
    explicit Texture2D(const std::string& path);
    // From pixels already in memory, for procedurally generated tiles. rgba
    // must hold width * height * 4 bytes, bottom row first to match the
    // flipped load above.
    Texture2D(int width, int height, const unsigned char* rgba, const char* label);
    ~Texture2D();
    Texture2D(const Texture2D&) = delete;
    Texture2D& operator=(const Texture2D&) = delete;

    void bind(GLuint unit) const { glBindTextureUnit(unit, id_); }

private:
    GLuint id_ = 0;
};

} // namespace dvk::gl
