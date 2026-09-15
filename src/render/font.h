#pragma once

#include "render/gl.h"

#include <cstdint>
#include <vector>

namespace dvk::gl::font {

// The project's only typeface: a 5x7 bitmap set of capitals, digits and a few
// marks, generated at startup rather than loaded from a font library or a
// shipped atlas. It is what the HUD, the menus and the door signs all draw
// with, so it lives here rather than inside any one of them.
//
// Only 32..90 exist, which is space through 'Z'. Lower case is folded up by
// upper() below; anything else draws nothing.
inline constexpr int kGlyphW = 5;
inline constexpr int kGlyphH = 7;
inline constexpr int kFirstChar = 32;   // space
inline constexpr int kLastChar = 90;    // 'Z'
inline constexpr int kGlyphCount = kLastChar - kFirstChar + 1;
inline constexpr int kAtlasCols = 16;
inline constexpr int kAtlasRows = (kGlyphCount + kAtlasCols - 1) / kAtlasCols;

// One pixel of padding around each cell, so linear filtering at small scales
// does not bleed neighbouring glyphs into each other.
inline constexpr int kCellW = kGlyphW + 1;
inline constexpr int kCellH = kGlyphH + 1;
inline constexpr int kAtlasW = kAtlasCols * kCellW;
inline constexpr int kAtlasH = kAtlasRows * kCellH;

// Left edge to left edge of the next glyph, in the same units kGlyphW is: one
// blank column between letters.
inline constexpr int kAdvance = kGlyphW + 1;

// The ink width of a run of n glyphs, in those same units. The last glyph
// carries no trailing gap, which is the difference between a string that looks
// centred and one that looks a half-space to the left.
constexpr float inkWidth(size_t glyphs) {
    return glyphs == 0 ? 0.0f
                       : static_cast<float>((glyphs - 1) * kAdvance + kGlyphW);
}

// A GL_TEXTURE_2D of the whole atlas: white with full alpha where a glyph
// covers, transparent black everywhere else. Linear-filtered and clamped, so
// it is ready to bind. The caller owns it and must delete it.
GLuint makeTexture();

// The atlas UV rectangle of one glyph, with v0 at the TOP of the cell. False
// when the font has no such character, in which case nothing should be drawn.
bool glyphUv(char c, float& u0, float& v0, float& u1, float& v1);

// Folds the one range the font is missing onto the one it has.
constexpr char upper(char c) {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c;
}

} // namespace dvk::gl::font
