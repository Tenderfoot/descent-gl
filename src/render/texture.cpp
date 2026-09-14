#include "render/texture.h"

#include "core/log.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>

namespace dvk::gl {

Texture2D::Texture2D(const std::string& path) {
    stbi_set_flip_vertically_on_load(1);
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels) log::fatal("cannot load texture '{}': {}", path, stbi_failure_reason());

    const auto levels = static_cast<GLsizei>(
        1 + std::floor(std::log2(static_cast<double>(std::max(w, h)))));

    glCreateTextures(GL_TEXTURE_2D, 1, &id_);
    glTextureStorage2D(id_, levels, GL_RGBA8, w, h);
    glTextureSubImage2D(id_, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glGenerateTextureMipmap(id_);

    glTextureParameteri(id_, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(id_, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTextureParameteri(id_, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(id_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Anisotropic filtering is core since GL 4.6, so no extension check.
    GLfloat maxAniso = 1.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
    glTextureParameterf(id_, GL_TEXTURE_MAX_ANISOTROPY, std::min(8.0f, maxAniso));

    stbi_image_free(pixels);
    log::info("texture '{}' {}x{} ({} levels)", path, w, h, levels);
}

Texture2D::Texture2D(int width, int height, const unsigned char* rgba, const char* label) {
    const auto levels = static_cast<GLsizei>(
        1 + std::floor(std::log2(static_cast<double>(std::max(width, height)))));

    glCreateTextures(GL_TEXTURE_2D, 1, &id_);
    glTextureStorage2D(id_, levels, GL_RGBA8, width, height);
    glTextureSubImage2D(id_, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glGenerateTextureMipmap(id_);

    glTextureParameteri(id_, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(id_, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTextureParameteri(id_, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(id_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    GLfloat maxAniso = 1.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
    glTextureParameterf(id_, GL_TEXTURE_MAX_ANISOTROPY, std::min(8.0f, maxAniso));

    log::info("texture '{}' {}x{} ({} levels, generated)", label, width, height, levels);
}

Texture2D::~Texture2D() {
    if (id_) glDeleteTextures(1, &id_);
}

} // namespace dvk::gl
