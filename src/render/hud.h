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
// Text comes from a procedurally generated 5x7 bitmap font baked into one
// texture at startup, rather than from a font library or a shipped atlas. The
// HUD needs digits, a colon and capitals; that is 37 glyphs, and generating
// them keeps the dependency list where CLAUDE.md says it is.
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
    void text(std::string_view text, float x, float y, float scale, const glm::vec4& color);
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

    void glyph(char c, float x, float y, float scale, const glm::vec4& color);

    std::vector<Vertex> vertices_;
    GLuint vao_ = 0, vbo_ = 0, font_ = 0;
    size_t capacity_ = 0;
    int width_ = 1, height_ = 1;
};

} // namespace dvk::gl
