#include "render/procedural.h"

#include <algorithm>
#include <cmath>

namespace dvk::gl {

std::vector<uint8_t> makeGridTile(int size, int cells) {
    std::vector<uint8_t> pixels(static_cast<size_t>(size) * static_cast<size_t>(size) * 4);

    const int cell = std::max(1, size / std::max(1, cells));
    const int line = std::max(1, size / 128);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int cx = x % cell;
            const int cy = y % cell;
            const bool onLine = cx < line || cy < line;

            // A faint checker under the grid so that a surface still reads as
            // moving when a grid line is not in view.
            const bool darkCell = ((x / cell) + (y / cell)) % 2 == 0;
            uint8_t base = darkCell ? 82 : 96;

            uint8_t r = base, g = base, b = static_cast<uint8_t>(base + 14);
            if (onLine) {
                r = 150;
                g = 175;
                b = 205;
            }

            const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(size) +
                              static_cast<size_t>(x)) * 4;
            pixels[i + 0] = r;
            pixels[i + 1] = g;
            pixels[i + 2] = b;
            pixels[i + 3] = 255;
        }
    }
    return pixels;
}

std::vector<uint8_t> makeHullTile(int size) {
    std::vector<uint8_t> pixels(static_cast<size_t>(size) * static_cast<size_t>(size) * 4);

    const int plate = std::max(1, size / 4);
    const int seam = std::max(1, size / 96);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            // Cheap hash per plate so neighbouring plates differ slightly and
            // the hull does not read as one flat colour.
            const int px = x / plate;
            const int py = y / plate;
            const unsigned h = static_cast<unsigned>(px * 73856093 ^ py * 19349663);
            const int jitter = static_cast<int>(h % 17u) - 8;

            int r = 104 + jitter;
            int g = 110 + jitter;
            int b = 122 + jitter;

            if (x % plate < seam || y % plate < seam) {
                r = 52;
                g = 56;
                b = 64;
            }

            // One warm stripe across the tile.
            const int band = size / 2;
            if (y >= band && y < band + std::max(2, size / 42)) {
                r = 198;
                g = 126;
                b = 44;
            }

            const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(size) +
                              static_cast<size_t>(x)) * 4;
            pixels[i + 0] = static_cast<uint8_t>(std::clamp(r, 0, 255));
            pixels[i + 1] = static_cast<uint8_t>(std::clamp(g, 0, 255));
            pixels[i + 2] = static_cast<uint8_t>(std::clamp(b, 0, 255));
            pixels[i + 3] = 255;
        }
    }
    return pixels;
}

} // namespace dvk::gl
