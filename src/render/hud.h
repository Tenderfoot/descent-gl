#pragma once

#include "render/gl.h"

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <string_view>
#include <vector>

namespace dvk::gl {

class Texture2D;

// The in-game HUD: screen-space quads drawn in the unlit pass.
//
// Text comes from the project's 5x7 bitmap font, generated into one texture at
// startup rather than loaded from a font library or a shipped atlas - see
// render/font.h, which the door signs draw from as well.
class Hud {
public:
    Hud();
    ~Hud();
    Hud(const Hud&) = delete;
    Hud& operator=(const Hud&) = delete;

    void begin(int width, int height);

    // Coordinates are pixels from the top-left, which is how a HUD is
    // naturally laid out; the shader flips into clip space.
    void rect(float x, float y, float w, float h, const glm::vec4& color);
    // A bar with its own frame, `fill` in 0..1.
    void bar(float x, float y, float w, float h, float fill, const glm::vec4& color);
    // One layer from the optional HUD image texture array. Layer selection is
    // encoded into UVs so the existing compact vertex format stays unchanged.
    void image(float x, float y, float w, float h, int layer, const glm::vec4& color);
    // The menu logo, which is one picture rather than a layer of the image
    // array: it has its own shape, and the array's layers are all the size of a
    // comm frame. Same trick for selecting it - a UV band no other quad uses.
    void logo(float x, float y, float w, float h, const glm::vec4& color);
    void text(std::string_view text, float x, float y, float scale, const glm::vec4& color);
    // The same, with everything outside [clipLeft, clipRight) cut away. A
    // glyph straddling an edge is trimmed rather than dropped, so a string
    // scrolling through a window slides instead of stepping a character at a
    // time. Everything the HUD draws shares one draw call, so this is done to
    // the quads rather than with a scissor.
    void textClipped(std::string_view text, float x, float y, float scale,
                     const glm::vec4& color, float clipLeft, float clipRight);
    float textWidth(std::string_view text, float scale) const;

    void upload();
    void draw() const;

    int quadCount() const { return static_cast<int>(vertices_.size() / 6); }

private:
    struct Vertex {
        glm::vec2 position;   // pixels
        glm::vec2 uv;
        glm::vec4 color;
    };

    void glyph(char c, float x, float y, float scale, const glm::vec4& color,
               float clipLeft, float clipRight);

    std::vector<Vertex> vertices_;
    GLuint vao_ = 0, vbo_ = 0, font_ = 0;
    size_t capacity_ = 0;
    int width_ = 1, height_ = 1;
};

} // namespace dvk::gl
