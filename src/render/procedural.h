#pragma once

#include <cstdint>
#include <vector>

namespace dvk::gl {

// Procedurally generated texture tiles, so the project can look like something
// without asset work. M1 needs one grid; M2 generates the mine's wall set the
// same way into a texture array.
//
// Returns width * height * 4 bytes of RGBA.
std::vector<uint8_t> makeGridTile(int size, int cells);

// Panelled metal for the ship hull: plate seams, a little per-plate variation,
// and a warm stripe so roll is readable from the chase camera.
std::vector<uint8_t> makeHullTile(int size);

} // namespace dvk::gl
