#include "render/hud.h"

#include "render/font.h"

#include <glm/vec2.hpp>

namespace dvk::gl {

Hud::Hud() {
    font_ = font::makeTexture();

    glCreateVertexArrays(1, &vao_);
    glCreateBuffers(1, &vbo_);
    glVertexArrayVertexBuffer(vao_, 0, vbo_, 0, sizeof(Vertex));

    glEnableVertexArrayAttrib(vao_, 0);
    glVertexArrayAttribFormat(vao_, 0, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, position));
    glVertexArrayAttribBinding(vao_, 0, 0);

    glEnableVertexArrayAttrib(vao_, 1);
    glVertexArrayAttribFormat(vao_, 1, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, uv));
    glVertexArrayAttribBinding(vao_, 1, 0);

    glEnableVertexArrayAttrib(vao_, 2);
    glVertexArrayAttribFormat(vao_, 2, 4, GL_FLOAT, GL_FALSE, offsetof(Vertex, color));
    glVertexArrayAttribBinding(vao_, 2, 0);
}

Hud::~Hud() {
    if (font_) glDeleteTextures(1, &font_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void Hud::begin(int width, int height) {
    vertices_.clear();
    width_ = width > 0 ? width : 1;
    height_ = height > 0 ? height : 1;
}

void Hud::rect(float x, float y, float w, float h, const glm::vec4& color) {
    // UV (-1,-1) is the sentinel the shader reads as "no texture", so solid
    // quads and glyphs can share one draw call.
    const glm::vec2 none(-1.0f, -1.0f);
    const Vertex a{{x, y}, none, color};
    const Vertex b{{x + w, y}, none, color};
    const Vertex c{{x + w, y + h}, none, color};
    const Vertex d{{x, y + h}, none, color};
    vertices_.push_back(a);
    vertices_.push_back(b);
    vertices_.push_back(c);
    vertices_.push_back(a);
    vertices_.push_back(c);
    vertices_.push_back(d);
}

void Hud::bar(float x, float y, float w, float h, float fill, const glm::vec4& color) {
    constexpr float kFrame = 1.0f;
    rect(x - kFrame, y - kFrame, w + 2 * kFrame, h + 2 * kFrame,
         glm::vec4(color.r * 0.35f, color.g * 0.35f, color.b * 0.35f, 0.85f));
    rect(x, y, w, h, glm::vec4(0.0f, 0.0f, 0.0f, 0.6f));
    const float clamped = fill < 0.0f ? 0.0f : (fill > 1.0f ? 1.0f : fill);
    if (clamped > 0.0f) rect(x, y, w * clamped, h, color);
}

void Hud::glyph(char c, float x, float y, float scale, const glm::vec4& color) {
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
    if (!font::glyphUv(c, u0, v0, u1, v1)) return;

    const float w = static_cast<float>(font::kGlyphW) * scale;
    const float h = static_cast<float>(font::kGlyphH) * scale;

    // y grows downward here, which is the same direction v does in the atlas,
    // so the glyph's top row lands on the quad's top edge with no flip.
    const Vertex v0v{{x, y}, {u0, v0}, color};
    const Vertex v1v{{x + w, y}, {u1, v0}, color};
    const Vertex v2v{{x + w, y + h}, {u1, v1}, color};
    const Vertex v3v{{x, y + h}, {u0, v1}, color};
    vertices_.push_back(v0v);
    vertices_.push_back(v1v);
    vertices_.push_back(v2v);
    vertices_.push_back(v0v);
    vertices_.push_back(v2v);
    vertices_.push_back(v3v);
}

void Hud::text(std::string_view text, float x, float y, float scale, const glm::vec4& color) {
    float cursor = x;
    for (char c : text) {
        glyph(font::upper(c), cursor, y, scale, color);
        cursor += static_cast<float>(font::kAdvance) * scale;
    }
}

float Hud::textWidth(std::string_view text, float scale) const {
    return static_cast<float>(text.size() * font::kAdvance) * scale;
}

void Hud::upload() {
    if (vertices_.empty()) return;
    const size_t bytes = vertices_.size() * sizeof(Vertex);
    if (bytes > capacity_) {
        capacity_ = bytes + bytes / 2;
        glNamedBufferData(vbo_, static_cast<GLsizeiptr>(capacity_), nullptr, GL_STREAM_DRAW);
    }
    glNamedBufferSubData(vbo_, 0, static_cast<GLsizeiptr>(bytes), vertices_.data());
}

void Hud::draw() const {
    if (vertices_.empty()) return;
    glBindTextureUnit(1, font_);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices_.size()));
}

} // namespace dvk::gl
