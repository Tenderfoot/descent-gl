#pragma once

#include <cstdint>
#include <vector>

namespace dvk::gl {

// Procedurally generated texture tiles, so the project can look like something
// without asset work. Each returns width * height * 4 bytes of RGBA.

// Panelled metal for the ship hull: plate seams, a little per-plate variation,
// and a warm stripe so roll is readable from the chase camera.
std::vector<uint8_t> makeHullTile(int size);

// The mine's wall set, in the order the level format's texture indices use.
// Kept together so a level file's `texture: 2` means the same thing everywhere.
enum WallTile : int {
    WallRock = 0,
    WallPanel,
    WallHazard,
    WallGrate,
    WallHull,     // the ship, so one texture array covers everything lit
    WallTileCount,
};

// All WallTileCount tiles at the given size, ready for a TextureArray.
std::vector<std::vector<uint8_t>> makeWallTiles(int size);

} // namespace dvk::gl
