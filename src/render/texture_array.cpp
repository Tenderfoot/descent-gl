#include "render/texture_array.h"

#include "core/log.h"

#include <algorithm>
#include <cmath>

namespace dvk::gl {

TextureArray::TextureArray(int size, const std::vector<std::vector<uint8_t>>& layers,
                           const char* label, Wrap wrap)
    : TextureArray(size, size, layers, label, wrap) {}

TextureArray::TextureArray(int width, int height,
                           const std::vector<std::vector<uint8_t>>& layers,
                           const char* label, Wrap wrap)
    : layers_(static_cast<int>(layers.size())) {
    if (layers.empty()) log::fatal("texture array '{}' has no layers", label);

    const int longest = std::max(width, height);
    const auto levels = static_cast<GLsizei>(
        1 + std::floor(std::log2(static_cast<double>(longest))));

    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &id_);
    glTextureStorage3D(id_, levels, GL_RGBA8, width, height, layers_);

    for (int i = 0; i < layers_; ++i) {
        const auto& pixels = layers[static_cast<size_t>(i)];
        const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
        if (pixels.size() != expected) {
            log::fatal("texture array '{}' layer {} is {} bytes, expected {}", label, i,
                       pixels.size(), expected);
        }
        glTextureSubImage3D(id_, 0, 0, 0, i, width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                            pixels.data());
    }
    glGenerateTextureMipmap(id_);

    const GLint mode = wrap == Wrap::Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTextureParameteri(id_, GL_TEXTURE_WRAP_S, mode);
    glTextureParameteri(id_, GL_TEXTURE_WRAP_T, mode);
    glTextureParameteri(id_, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(id_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    GLfloat maxAniso = 1.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
    glTextureParameterf(id_, GL_TEXTURE_MAX_ANISOTROPY, std::min(8.0f, maxAniso));

    log::info("texture array '{}' {}x{} x {} layers ({} levels)", label, width, height, layers_,
              levels);
}

TextureArray::~TextureArray() {
    if (id_) glDeleteTextures(1, &id_);
}

} // namespace dvk::gl
