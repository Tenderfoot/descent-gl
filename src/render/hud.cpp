#include "render/hud.h"

#include "core/log.h"

#include <array>
#include <cstdint>

namespace dvk::gl {

namespace {

// 5x7 glyphs, one bit per pixel, row 0 at the top. Only what the HUD needs:
// digits, a colon, a percent-free set of capitals and a few marks.
constexpr int kGlyphW = 5;
constexpr int kGlyphH = 7;
constexpr int kFirstChar = 32;   // space
constexpr int kLastChar = 90;    // 'Z'
constexpr int kGlyphCount = kLastChar - kFirstChar + 1;
constexpr int kAtlasCols = 16;
constexpr int kAtlasRows = (kGlyphCount + kAtlasCols - 1) / kAtlasCols;

// Each glyph is seven bytes, one per row, low five bits used.
constexpr std::array<std::array<uint8_t, 7>, kGlyphCount> kFont{{
    {{0,0,0,0,0,0,0}},                          // space
    {{4,4,4,4,4,0,4}},                          // !
    {{10,10,0,0,0,0,0}},                        // "
    {{10,31,10,10,31,10,0}},                    // #
    {{4,15,20,14,5,30,4}},                      // $
    {{25,26,2,4,8,11,19}},                      // %
    {{12,18,20,8,21,18,13}},                    // &
    {{4,4,0,0,0,0,0}},                          // '
    {{2,4,8,8,8,4,2}},                          // (
    {{8,4,2,2,2,4,8}},                          // )
    {{0,4,21,14,21,4,0}},                       // *
    {{0,4,4,31,4,4,0}},                         // +
    {{0,0,0,0,4,4,8}},                          // ,
    {{0,0,0,31,0,0,0}},                         // -
    {{0,0,0,0,0,12,12}},                        // .
    {{1,2,2,4,8,8,16}},                         // /
    {{14,17,19,21,25,17,14}},                   // 0
    {{4,12,4,4,4,4,14}},                        // 1
    {{14,17,1,2,4,8,31}},                       // 2
    {{31,2,4,2,1,17,14}},                       // 3
    {{2,6,10,18,31,2,2}},                       // 4
    {{31,16,30,1,1,17,14}},                     // 5
    {{6,8,16,30,17,17,14}},                     // 6
    {{31,1,2,4,8,8,8}},                         // 7
    {{14,17,17,14,17,17,14}},                   // 8
    {{14,17,17,15,1,2,12}},                     // 9
    {{0,12,12,0,12,12,0}},                      // :
    {{0,12,12,0,12,4,8}},                       // ;
    {{2,4,8,16,8,4,2}},                         // <
    {{0,0,31,0,31,0,0}},                        // =
    {{8,4,2,1,2,4,8}},                          // >
    {{14,17,1,2,4,0,4}},                        // ?
    {{14,17,1,13,21,21,14}},                    // @
    {{14,17,17,31,17,17,17}},                   // A
    {{30,17,17,30,17,17,30}},                   // B
    {{14,17,16,16,16,17,14}},                   // C
    {{28,18,17,17,17,18,28}},                   // D
    {{31,16,16,30,16,16,31}},                   // E
    {{31,16,16,30,16,16,16}},                   // F
    {{14,17,16,23,17,17,15}},                   // G
    {{17,17,17,31,17,17,17}},                   // H
    {{14,4,4,4,4,4,14}},                        // I
    {{7,2,2,2,2,18,12}},                        // J
    {{17,18,20,24,20,18,17}},                   // K
    {{16,16,16,16,16,16,31}},                   // L
    {{17,27,21,21,17,17,17}},                   // M
    {{17,17,25,21,19,17,17}},                   // N
    {{14,17,17,17,17,17,14}},                   // O
    {{30,17,17,30,16,16,16}},                   // P
    {{14,17,17,17,21,18,13}},                   // Q
    {{30,17,17,30,20,18,17}},                   // R
    {{15,16,16,14,1,1,30}},                     // S
    {{31,4,4,4,4,4,4}},                         // T
    {{17,17,17,17,17,17,14}},                   // U
    {{17,17,17,17,17,10,4}},                    // V
    {{17,17,17,21,21,21,10}},                   // W
    {{17,17,10,4,10,17,17}},                    // X
    {{17,17,10,4,4,4,4}},                       // Y
    {{31,1,2,4,8,16,31}},                       // Z
}};

} // namespace

Hud::Hud() {
    // One pixel of padding around each cell, so linear filtering at small
    // scales does not bleed neighbouring glyphs into each other.
    constexpr int kCellW = kGlyphW + 1;
    constexpr int kCellH = kGlyphH + 1;
    const int atlasW = kAtlasCols * kCellW;
    const int atlasH = kAtlasRows * kCellH;

    std::vector<uint8_t> pixels(static_cast<size_t>(atlasW) * static_cast<size_t>(atlasH) * 4, 0);
    for (int g = 0; g < kGlyphCount; ++g) {
        const int cx = (g % kAtlasCols) * kCellW;
        const int cy = (g / kAtlasCols) * kCellH;
        for (int row = 0; row < kGlyphH; ++row) {
            const uint8_t bits = kFont[static_cast<size_t>(g)][static_cast<size_t>(row)];
            for (int col = 0; col < kGlyphW; ++col) {
                if (((bits >> (kGlyphW - 1 - col)) & 1) == 0) continue;
                const size_t i = (static_cast<size_t>(cy + row) * static_cast<size_t>(atlasW) +
                                  static_cast<size_t>(cx + col)) * 4;
                pixels[i + 0] = 255;
                pixels[i + 1] = 255;
                pixels[i + 2] = 255;
                pixels[i + 3] = 255;
            }
        }
    }

    glCreateTextures(GL_TEXTURE_2D, 1, &font_);
    glTextureStorage2D(font_, 1, GL_RGBA8, atlasW, atlasH);
    glTextureSubImage2D(font_, 0, 0, 0, atlasW, atlasH, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glTextureParameteri(font_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(font_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(font_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(font_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

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

    log::info("hud font atlas {}x{} ({} glyphs, generated)", atlasW, atlasH, kGlyphCount);
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
    if (c < kFirstChar || c > kLastChar) return;
    const int index = c - kFirstChar;
    constexpr int kCellW = kGlyphW + 1;
    constexpr int kCellH = kGlyphH + 1;
    const float atlasW = static_cast<float>(kAtlasCols * kCellW);
    const float atlasH = static_cast<float>(kAtlasRows * kCellH);

    const float u0 = static_cast<float>((index % kAtlasCols) * kCellW) / atlasW;
    const float v0 = static_cast<float>((index / kAtlasCols) * kCellH) / atlasH;
    const float u1 = u0 + static_cast<float>(kGlyphW) / atlasW;
    const float v1 = v0 + static_cast<float>(kGlyphH) / atlasH;

    const float w = static_cast<float>(kGlyphW) * scale;
    const float h = static_cast<float>(kGlyphH) * scale;

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
        const char upper = c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c;
        glyph(upper, cursor, y, scale, color);
        cursor += static_cast<float>(kGlyphW + 1) * scale;
    }
}

float Hud::textWidth(std::string_view text, float scale) const {
    return static_cast<float>(text.size() * (kGlyphW + 1)) * scale;
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
