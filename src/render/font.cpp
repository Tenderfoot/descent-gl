#include "render/font.h"

#include <array>

namespace dvk::gl::font {

namespace {

// Each glyph is seven bytes, one per row, low five bits used, row 0 at the top.
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

GLuint makeTexture() {
    std::vector<uint8_t> pixels(
        static_cast<size_t>(kAtlasW) * static_cast<size_t>(kAtlasH) * 4, 0);
    for (int g = 0; g < kGlyphCount; ++g) {
        const int cx = (g % kAtlasCols) * kCellW;
        const int cy = (g / kAtlasCols) * kCellH;
        for (int row = 0; row < kGlyphH; ++row) {
            const uint8_t bits = kFont[static_cast<size_t>(g)][static_cast<size_t>(row)];
            for (int col = 0; col < kGlyphW; ++col) {
                if (((bits >> (kGlyphW - 1 - col)) & 1) == 0) continue;
                const size_t i = (static_cast<size_t>(cy + row) * static_cast<size_t>(kAtlasW) +
                                  static_cast<size_t>(cx + col)) * 4;
                pixels[i + 0] = 255;
                pixels[i + 1] = 255;
                pixels[i + 2] = 255;
                pixels[i + 3] = 255;
            }
        }
    }

    GLuint texture = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &texture);
    glTextureStorage2D(texture, 1, GL_RGBA8, kAtlasW, kAtlasH);
    glTextureSubImage2D(texture, 0, 0, 0, kAtlasW, kAtlasH, GL_RGBA, GL_UNSIGNED_BYTE,
                        pixels.data());
    glTextureParameteri(texture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(texture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(texture, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(texture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return texture;
}

bool glyphUv(char c, float& u0, float& v0, float& u1, float& v1) {
    if (c < kFirstChar || c > kLastChar) return false;
    const int index = c - kFirstChar;
    constexpr float atlasW = static_cast<float>(kAtlasW);
    constexpr float atlasH = static_cast<float>(kAtlasH);

    u0 = static_cast<float>((index % kAtlasCols) * kCellW) / atlasW;
    v0 = static_cast<float>((index / kAtlasCols) * kCellH) / atlasH;
    u1 = u0 + static_cast<float>(kGlyphW) / atlasW;
    v1 = v0 + static_cast<float>(kGlyphH) / atlasH;
    return true;
}

} // namespace dvk::gl::font
