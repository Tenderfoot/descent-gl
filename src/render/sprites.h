#pragma once

#include <cstdint>
#include <vector>

namespace dvk::gl {

// Hand-authored sprite art, loaded from assets/images/ and handed to a
// TextureArray. The counterpart to procedural.h: that file makes tiles because
// nobody drew any, this one reads the ones somebody did.

// The pickup sprite set, in the order the texture array's layers use. A pickup
// kind maps to one of these in the renderer; kinds with no art here keep the
// procedural radial glow instead.
enum PickupSprite : int {
    SpriteHealth = 0,
    SpriteEnergy,
    SpriteLaser,
    SpriteConcussion,
    SpriteHoming,
    SpriteVulcan,
    SpriteRailgun,
    SpriteLightning,
    SpriteKey,      // a white template, tinted per key colour at draw time
    PickupSpriteCount,
};

// All PickupSpriteCount sprites, square and `size` on a side, ready for a
// TextureArray. Each is size * size * 4 bytes of RGBA with the colour already
// multiplied by the alpha - see loadSpriteLayers() on why that matters.
std::vector<std::vector<uint8_t>> loadPickupSprites(int size);

// One image at its own size, for art that is not a square sprite.
struct LoadedImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

// The wordmark that tops the main menu, copied from marketing/steam/.
//
// Trimmed to its opaque bounds, because a Steam library logo is padded out to a
// fixed canvas and that padding is not part of the picture: centring the canvas
// would put the wordmark wherever the exporter happened to leave room. What
// comes back is the wordmark itself, so the menu can centre it and size it by
// its own aspect rather than by numbers transcribed from this particular
// export.
//
// Straight alpha, unlike the pickup sprites: the HUD blends with
// GL_SRC_ALPHA/GL_ONE_MINUS_SRC_ALPHA rather than additively.
LoadedImage loadMenuLogo(const char* path);

} // namespace dvk::gl
