#pragma once

#include "render/gl.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <string_view>
#include <vector>

namespace dvk::gl {

// Lit signage bolted to the door slabs: the strips that tell the player whether
// the door in front of them is going to open, and which key it wants.
//
// The world-space sibling of Hud, and deliberately built the same way - a bezel
// quad, a plate quad on top of it and one quad per letter, all in one stream
// buffer and one draw. It shares the HUD's font atlas and its fragment shader,
// which already reads a negative UV as "solid quad, no texture"; only the
// vertex stage differs, because these quads are in the mine rather than on the
// screen. See shaders/sign.vert.
//
// Drawn in the unlit pass at full brightness. A mine with no ambient term has
// nowhere else to put a sign that has to be legible in an unlit corridor, and
// an illuminated sign is what a mine would have anyway.
class DoorSigns {
public:
    DoorSigns();
    ~DoorSigns();
    DoorSigns(const DoorSigns&) = delete;
    DoorSigns& operator=(const DoorSigns&) = delete;

    // Every quad the pass can ever be asked for, up front: growing the buffer
    // mid-frame is a heap allocation in the draw path, and the frame report
    // says so.
    void reserveQuads(size_t quads) { vertices_.reserve(quads * 6); }

    void clear() { vertices_.clear(); }
    bool empty() const { return vertices_.empty(); }

    // Where one piece of a sign lives, and how much of that sign it carries.
    //
    // The sign is laid out across a rectangle 2*halfWidth by 2*halfHeight
    // centred on `center`, in the frame `right` and `up` span - but only the
    // horizontal slice [uMin, uMax] of it is emitted, and the caller is free to
    // put that slice wherever it likes by moving `center`. That is what lets
    // one strip span two door halves that slide apart: each half asks for its
    // own slice at its own position, and a letter straddling the seam is cut in
    // two rather than drawn twice or squeezed to fit.
    //
    // `right` and `up` must be unit and orthogonal, and `right` is the
    // direction the text runs in AS THE READER SEES IT. A panel on the far side
    // of a door therefore passes the negated `right`: the sign occupies the
    // same rectangle in the world, and the lettering still reads left to right
    // from over there.
    struct Panel {
        glm::vec3 center{0.0f};
        glm::vec3 right{1.0f, 0.0f, 0.0f};
        glm::vec3 up{0.0f, 1.0f, 0.0f};
        float halfWidth = 1.0f;
        float halfHeight = 0.25f;
        float uMin = 0.0f;
        float uMax = 1.0f;
    };

    // `plate` fills the panel, `ink` writes the text across it, and the bezel
    // is a darkened `plate` showing at the panel's edge. The text is centred
    // and scaled to fit the plate, so the caller sizes the sign and not the
    // lettering.
    void add(const Panel& panel, std::string_view text, const glm::vec4& plate,
             const glm::vec4& ink);

    void upload();
    void draw() const;

    int quadCount() const { return static_cast<int>(vertices_.size() / 6); }

private:
    struct Vertex {
        glm::vec3 position;
        glm::vec2 uv;
        glm::vec4 color;
    };

    // In the panel's own 2D frame: x across and y up, both in world units
    // measured from the panel centre. `uvMin`/`uvMax` are atlas coordinates
    // with v at uvMin on the glyph's TOP row, or the (-1,-1) sentinel the
    // shader reads as "solid quad".
    void quad(const Panel& panel, float x0, float y0, float x1, float y1, const glm::vec2& uvMin,
              const glm::vec2& uvMax, const glm::vec4& color);

    std::vector<Vertex> vertices_;
    GLuint vao_ = 0, vbo_ = 0, font_ = 0;
    size_t capacity_ = 0;
};

} // namespace dvk::gl
