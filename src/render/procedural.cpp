#include "render/procedural.h"

#include <algorithm>
#include <cmath>

namespace dvk::gl {

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

namespace {

// Deterministic value noise. Not a good noise function; good enough to break up
// a flat colour, and identical from run to run so a screenshot can be compared
// against an earlier one.
float hashNoise(int x, int y, int seed) {
    unsigned h = static_cast<unsigned>(x) * 374761393u + static_cast<unsigned>(y) * 668265263u +
                 static_cast<unsigned>(seed) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f;
}

float smoothNoise(float x, float y, int seed) {
    const int xi = static_cast<int>(std::floor(x));
    const int yi = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(xi);
    const float fy = y - static_cast<float>(yi);
    const float sx = fx * fx * (3.0f - 2.0f * fx);
    const float sy = fy * fy * (3.0f - 2.0f * fy);

    const float a = hashNoise(xi, yi, seed);
    const float b = hashNoise(xi + 1, yi, seed);
    const float c = hashNoise(xi, yi + 1, seed);
    const float d = hashNoise(xi + 1, yi + 1, seed);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

void put(std::vector<uint8_t>& px, int size, int x, int y, int r, int g, int b) {
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(size) +
                      static_cast<size_t>(x)) * 4;
    px[i + 0] = static_cast<uint8_t>(std::clamp(r, 0, 255));
    px[i + 1] = static_cast<uint8_t>(std::clamp(g, 0, 255));
    px[i + 2] = static_cast<uint8_t>(std::clamp(b, 0, 255));
    px[i + 3] = 255;
}

// Tiles are authored bright. With no ambient term the only thing that darkens a
// surface is distance from a light, so an albedo that already looks "dark and
// cave-like" would render as pure black almost everywhere.
std::vector<uint8_t> makeRockTile(int size) {
    std::vector<uint8_t> px(static_cast<size_t>(size) * static_cast<size_t>(size) * 4);
    const float scale = 8.0f / static_cast<float>(size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float fx = static_cast<float>(x) * scale;
            const float fy = static_cast<float>(y) * scale;
            float n = smoothNoise(fx, fy, 11) * 0.6f + smoothNoise(fx * 3.0f, fy * 3.0f, 12) * 0.3f +
                      smoothNoise(fx * 9.0f, fy * 9.0f, 13) * 0.1f;
            const int v = static_cast<int>(120.0f + n * 90.0f);
            put(px, size, x, y, v, static_cast<int>(v * 0.94f), static_cast<int>(v * 0.86f));
        }
    }
    return px;
}

std::vector<uint8_t> makePanelTile(int size) {
    std::vector<uint8_t> px(static_cast<size_t>(size) * static_cast<size_t>(size) * 4);
    const int plate = std::max(1, size / 4);
    const int seam = std::max(1, size / 64);
    const float scale = 16.0f / static_cast<float>(size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int pxi = x / plate;
            const int pyi = y / plate;
            const int jitter = static_cast<int>(hashNoise(pxi, pyi, 21) * 26.0f) - 13;
            const float grain = smoothNoise(static_cast<float>(x) * scale,
                                            static_cast<float>(y) * scale, 22) * 18.0f;
            int v = 150 + jitter + static_cast<int>(grain);
            int r = v, g = v + 4, b = v + 14;
            if (x % plate < seam || y % plate < seam) {
                r = 78;
                g = 84;
                b = 96;
            }
            // Rivets at the plate corners.
            const int rx = x % plate, ry = y % plate;
            const int rr = std::max(2, size / 64);
            const int cx = plate / 8, cy = plate / 8;
            if ((rx - cx) * (rx - cx) + (ry - cy) * (ry - cy) < rr * rr) {
                r = 196;
                g = 200;
                b = 210;
            }
            put(px, size, x, y, r, g, b);
        }
    }
    return px;
}

std::vector<uint8_t> makeHazardTile(int size) {
    std::vector<uint8_t> px(static_cast<size_t>(size) * static_cast<size_t>(size) * 4);
    const int stripe = std::max(1, size / 8);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            // Diagonal bands, wrapping so the tile still repeats seamlessly.
            const int band = ((x + y) % size) / stripe;
            const bool warn = band % 2 == 0;
            const float n = smoothNoise(static_cast<float>(x) * 0.09f,
                                        static_cast<float>(y) * 0.09f, 31);
            const int wear = static_cast<int>(n * 34.0f) - 17;
            if (warn) {
                put(px, size, x, y, 224 + wear, 168 + wear, 38 + wear);
            } else {
                put(px, size, x, y, 54 + wear, 52 + wear, 56 + wear);
            }
        }
    }
    return px;
}

std::vector<uint8_t> makeGrateTile(int size) {
    std::vector<uint8_t> px(static_cast<size_t>(size) * static_cast<size_t>(size) * 4);
    const int cell = std::max(2, size / 10);
    const int bar = std::max(2, cell / 3);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const bool onBar = (x % cell) < bar || (y % cell) < bar;
            if (onBar) {
                // Shade the bar across its width so it reads as round.
                const int across = std::min(x % cell, y % cell);
                const int shade = 130 + (across * 70) / std::max(1, bar);
                put(px, size, x, y, shade, shade + 4, shade + 12);
            } else {
                put(px, size, x, y, 34, 36, 42);
            }
        }
    }
    return px;
}

} // namespace

std::vector<std::vector<uint8_t>> makeWallTiles(int size) {
    std::vector<std::vector<uint8_t>> tiles(static_cast<size_t>(WallTileCount));
    tiles[WallRock] = makeRockTile(size);
    tiles[WallPanel] = makePanelTile(size);
    tiles[WallHazard] = makeHazardTile(size);
    tiles[WallGrate] = makeGrateTile(size);
    tiles[WallHull] = makeHullTile(size);
    return tiles;
}

} // namespace dvk::gl
