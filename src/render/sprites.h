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
    SpriteKey,      // a white template, tinted per key colour at draw time
    PickupSpriteCount,
};

// All PickupSpriteCount sprites, square and `size` on a side, ready for a
// TextureArray. Each is size * size * 4 bytes of RGBA with the colour already
// multiplied by the alpha - see loadSpriteLayers() on why that matters.
std::vector<std::vector<uint8_t>> loadPickupSprites(int size);

} // namespace dvk::gl
