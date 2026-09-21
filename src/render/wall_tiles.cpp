#include "render/wall_tiles.h"

#include "core/log.h"
#include "render/sprites.h"

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>

namespace dvk::gl {
namespace {

// The names the renderer asks for by name rather than by index, and the member
// of WallTileIndex each one fills. See the comment on that struct.
constexpr std::array<std::pair<const char*, int WallTileIndex::*>, 4> kRequired{{
    {"rock", &WallTileIndex::rock},
    {"panel", &WallTileIndex::panel},
    {"hazard", &WallTileIndex::hazard},
    {"hull", &WallTileIndex::hull},
}};

nlohmann::json readManifest(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) log::fatal("tile manifest '{}' would not open", path.string());
    nlohmann::json manifest;
    try {
        in >> manifest;
    } catch (const std::exception& e) {
        log::fatal("tile manifest '{}' is not valid JSON: {}", path.string(), e.what());
    }
    return manifest;
}

// One tile, square and RGBA, resampled to `size`.
std::vector<uint8_t> loadTile(const std::filesystem::path& path, int size) {
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load(path.string().c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        const char* why = stbi_failure_reason();
        log::fatal("tile '{}' would not load: {}", path.string(), why ? why : "no reason given");
    }
    // Fatal rather than corrected, the same way a sprite is: a texture array
    // layer is a fixed square, and stretching or upscaling art to fit it hides
    // the mistake in the asset until someone notices the wall looks wrong.
    if (w != h) log::fatal("tile '{}' is {}x{}; wall tiles must be square", path.string(), w, h);
    if (w < size) {
        log::fatal("tile '{}' is {}px, smaller than the {}px the set is built at. Supply larger "
                   "art, or lower \"size\" in the manifest for the whole set.",
                   path.string(), w, size);
    }

    const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<uint8_t> out = w == size ? std::vector<uint8_t>(pixels, pixels + count * 4)
                                         : downsample(pixels, w, size);
    stbi_image_free(pixels);
    if (w != size) log::info("tile '{}' {}x{} -> {}x{}", path.string(), w, h, size, size);
    return out;
}

}   // namespace

TileReport checkTile(const std::vector<uint8_t>& rgba, int size) {
    TileReport report;
    if (size <= 1 || rgba.size() != static_cast<size_t>(size) * size * 4) return report;

    // Everything below comes out of one pass over the pixels.
    //
    // The obvious shape - a columnDelta(a, b) helper called once per adjacent
    // pair - is a column scan inside a loop over columns, so it costs size^3.
    // That is unnoticeable at 256 and about a billion operations at 1024, which
    // is a --validate-assets that looks hung. Accumulating every pair's total
    // as the pass goes past it costs size^2 and gives the same numbers.
    std::vector<uint64_t> columnGap(static_cast<size_t>(size), 0);   // x to x + 1
    std::vector<uint64_t> rowGap(static_cast<size_t>(size), 0);      // y to y + 1
    uint64_t wrapColumn = 0;   // last column against the first
    uint64_t wrapRow = 0;      // last row against the first
    double luminance = 0.0;

    const auto at = [&](int x, int y) {
        return (static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)) * 4;
    };
    const auto absDiff = [&](size_t a, size_t b) {
        uint64_t sum = 0;
        for (size_t c = 0; c < 3; ++c) {
            sum += static_cast<uint64_t>(
                std::abs(static_cast<int>(rgba[a + c]) - static_cast<int>(rgba[b + c])));
        }
        return sum;
    };

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const size_t here = at(x, y);
            // Unweighted by gamma on purpose: nothing in this renderer converts
            // between sRGB and linear, so these bytes are what the lighting
            // math multiplies and what ends up on screen.
            luminance +=
                0.2126 * rgba[here] + 0.7152 * rgba[here + 1] + 0.0722 * rgba[here + 2];

            if (x + 1 < size) columnGap[static_cast<size_t>(x)] += absDiff(here, at(x + 1, y));
            if (y + 1 < size) rowGap[static_cast<size_t>(y)] += absDiff(here, at(x, y + 1));
        }
        wrapColumn += absDiff(at(size - 1, y), at(0, y));
    }
    for (int x = 0; x < size; ++x) wrapRow += absDiff(at(x, size - 1), at(x, 0));

    const auto mean = [&](uint64_t sum) {
        return static_cast<float>(sum) / static_cast<float>(size * 3);
    };
    report.meanLuminance =
        static_cast<float>(luminance / (static_cast<double>(size) * static_cast<double>(size)));

    float worstInteriorX = 0.0f;
    float worstInteriorY = 0.0f;
    for (int i = 0; i + 1 < size; ++i) {
        worstInteriorX = std::max(worstInteriorX, mean(columnGap[static_cast<size_t>(i)]));
        worstInteriorY = std::max(worstInteriorY, mean(rowGap[static_cast<size_t>(i)]));
    }

    // A flat tile has no interior edge to measure against and no seam either,
    // so the floor keeps the ratio from dividing its way to infinity.
    constexpr float kFloor = 0.5f;
    report.seamRatioX = mean(wrapColumn) / std::max(worstInteriorX, kFloor);
    report.seamRatioY = mean(wrapRow) / std::max(worstInteriorY, kFloor);
    return report;
}

int WallTiles::indexOf(std::string_view name) const {
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) return static_cast<int>(i);
    }
    return -1;
}

WallTiles loadWallTiles(const char* manifestPath) {
    const std::filesystem::path path(manifestPath);
    const nlohmann::json manifest = readManifest(path);
    const std::filesystem::path directory = path.parent_path();

    WallTiles tiles;
    tiles.size = manifest.value("size", 0);
    if (tiles.size <= 0) {
        log::fatal("tile manifest '{}' needs a positive \"size\"", path.string());
    }

    const auto list = manifest.find("tiles");
    if (list == manifest.end() || !list->is_array() || list->empty()) {
        log::fatal("tile manifest '{}' needs a non-empty \"tiles\" array", path.string());
    }

    // A PNG's first row is its top; a GL texture's first row is v = 0, which
    // level.cpp puts at the BOTTOM corner of a side. Flipping here is what lets
    // a tile look the same way up in an image editor as it does on the wall,
    // which is the whole point of the art being in files. The mirror of the
    // same call in loadPickupSprites().
    stbi_set_flip_vertically_on_load(1);

    for (const nlohmann::json& entry : *list) {
        if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string()) {
            log::fatal("tile manifest '{}': every entry needs a string \"name\"", path.string());
        }
        std::string name = entry["name"].get<std::string>();
        if (tiles.indexOf(name) >= 0) {
            log::fatal("tile manifest '{}' lists '{}' twice; a name selects one layer",
                       path.string(), name);
        }
        // `file` is optional so the common case is one word per line, and
        // present so a tile can be pointed at new art without the name - which
        // levels and the renderer depend on - having to change with it.
        const std::string file = entry.value("file", name + ".png");
        tiles.layers.push_back(loadTile(directory / file, tiles.size));
        tiles.names.push_back(std::move(name));
    }

    for (const auto& [name, member] : kRequired) {
        const int layer = tiles.indexOf(name);
        if (layer < 0) {
            log::fatal("tile manifest '{}' has no '{}'. The renderer selects that tile itself "
                       "rather than a level choosing it, so the set cannot go without one.",
                       path.string(), name);
        }
        tiles.index.*member = layer;
    }

    log::info("wall tiles: {} layer(s) at {}px from '{}'", tiles.layers.size(), tiles.size,
              directory.string());
    return tiles;
}

} // namespace dvk::gl
