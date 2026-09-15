#include "render/signs.h"

#include "render/font.h"

#include <algorithm>

namespace dvk::gl {

namespace {

// The bezel showing around the plate, as a fraction of the panel's half-height.
// The plate is the panel inset by this much on every side, so nothing a panel
// draws ever escapes the rectangle the caller asked for - which matters at the
// seam, where two panels meet edge to edge and any overhang would paint a dark
// bar down the middle of a shut door.
constexpr float kBezel = 0.16f;

// How much of the plate the lettering is allowed to fill.
constexpr float kTextWidthFill = 0.92f;
constexpr float kTextHeightFill = 0.66f;

// The sentinel hud.frag reads as "solid quad, no texture".
constexpr glm::vec2 kNoTexture{-1.0f, -1.0f};

} // namespace

DoorSigns::DoorSigns() {
    font_ = font::makeTexture();

    glCreateVertexArrays(1, &vao_);
    glCreateBuffers(1, &vbo_);
    glVertexArrayVertexBuffer(vao_, 0, vbo_, 0, sizeof(Vertex));

    glEnableVertexArrayAttrib(vao_, 0);
    glVertexArrayAttribFormat(vao_, 0, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, position));
    glVertexArrayAttribBinding(vao_, 0, 0);

    glEnableVertexArrayAttrib(vao_, 1);
    glVertexArrayAttribFormat(vao_, 1, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, uv));
    glVertexArrayAttribBinding(vao_, 1, 0);

    glEnableVertexArrayAttrib(vao_, 2);
    glVertexArrayAttribFormat(vao_, 2, 4, GL_FLOAT, GL_FALSE, offsetof(Vertex, color));
    glVertexArrayAttribBinding(vao_, 2, 0);
}

DoorSigns::~DoorSigns() {
    if (font_) glDeleteTextures(1, &font_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void DoorSigns::quad(const Panel& panel, float x0, float y0, float x1, float y1,
                     const glm::vec2& uvMin, const glm::vec2& uvMax, const glm::vec4& color) {
    const glm::vec3& r = panel.right;
    const glm::vec3& u = panel.up;

    // v runs down the atlas and y runs up the panel, so the quad's bottom edge
    // takes the glyph's bottom row. Getting this backwards draws the whole sign
    // upside down, which is the kind of thing that is obvious once seen.
    const Vertex a{panel.center + r * x0 + u * y0, {uvMin.x, uvMax.y}, color};
    const Vertex b{panel.center + r * x1 + u * y0, {uvMax.x, uvMax.y}, color};
    const Vertex c{panel.center + r * x1 + u * y1, {uvMax.x, uvMin.y}, color};
    const Vertex d{panel.center + r * x0 + u * y1, {uvMin.x, uvMin.y}, color};

    vertices_.push_back(a);
    vertices_.push_back(b);
    vertices_.push_back(c);
    vertices_.push_back(a);
    vertices_.push_back(c);
    vertices_.push_back(d);
}

void DoorSigns::add(const Panel& panel, std::string_view text, const glm::vec4& plate,
                    const glm::vec4& ink) {
    if (panel.halfWidth <= 0.0f || panel.halfHeight <= 0.0f) return;

    // The window this piece is allowed to draw in, in panel x.
    const float windowLo = (panel.uMin * 2.0f - 1.0f) * panel.halfWidth;
    const float windowHi = (panel.uMax * 2.0f - 1.0f) * panel.halfWidth;
    if (windowHi - windowLo <= 1e-4f) return;

    // A dark border, so the sign reads as something bolted to the door rather
    // than as a coloured hole cut in it.
    quad(panel, windowLo, -panel.halfHeight, windowHi, panel.halfHeight, kNoTexture, kNoTexture,
         glm::vec4(glm::vec3(plate) * 0.22f, plate.a));

    const float inset = panel.halfHeight * kBezel;
    const float plateLo = std::max(windowLo, -panel.halfWidth + inset);
    const float plateHi = std::min(windowHi, panel.halfWidth - inset);
    const float plateHalfH = panel.halfHeight - inset;
    if (plateHi - plateLo <= 1e-4f || plateHalfH <= 0.0f) return;

    quad(panel, plateLo, -plateHalfH, plateHi, plateHalfH, kNoTexture, kNoTexture, plate);

    if (text.empty()) return;

    // The lettering is laid out across the WHOLE plate, not across this piece
    // of it, so both halves of a split sign agree on where every letter goes.
    const float inkUnits = font::inkWidth(text.size());
    const float roomW = (panel.halfWidth - inset) * 2.0f * kTextWidthFill;
    const float roomH = plateHalfH * 2.0f * kTextHeightFill;
    const float scale = std::min(roomW / inkUnits, roomH / static_cast<float>(font::kGlyphH));
    if (scale <= 0.0f) return;

    const float glyphW = static_cast<float>(font::kGlyphW) * scale;
    const float glyphH = static_cast<float>(font::kGlyphH) * scale;
    const float advance = static_cast<float>(font::kAdvance) * scale;
    float cursor = -inkUnits * scale * 0.5f;

    for (char c : text) {
        const float x0 = cursor;
        const float x1 = cursor + glyphW;
        cursor += advance;

        float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
        if (!font::glyphUv(font::upper(c), u0, v0, u1, v1)) continue;

        // Clip to the plate rather than skipping or shifting: a letter sitting
        // on the seam of a door that is coming apart is split down the middle
        // and each half rides away with its own slab.
        const float cx0 = std::max(x0, plateLo);
        const float cx1 = std::min(x1, plateHi);
        if (cx1 - cx0 <= 1e-5f) continue;

        const float span = x1 - x0;
        const float cu0 = u0 + (u1 - u0) * (cx0 - x0) / span;
        const float cu1 = u0 + (u1 - u0) * (cx1 - x0) / span;
        quad(panel, cx0, -glyphH * 0.5f, cx1, glyphH * 0.5f, {cu0, v0}, {cu1, v1}, ink);
    }
}

void DoorSigns::upload() {
    if (vertices_.empty()) return;
    const size_t bytes = vertices_.size() * sizeof(Vertex);
    if (bytes > capacity_) {
        capacity_ = bytes + bytes / 2;
        glNamedBufferData(vbo_, static_cast<GLsizeiptr>(capacity_), nullptr, GL_STREAM_DRAW);
    }
    glNamedBufferSubData(vbo_, 0, static_cast<GLsizeiptr>(bytes), vertices_.data());
}

void DoorSigns::draw() const {
    if (vertices_.empty()) return;
    glBindTextureUnit(1, font_);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices_.size()));
}

} // namespace dvk::gl
