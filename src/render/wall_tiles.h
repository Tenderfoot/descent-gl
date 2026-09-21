#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dvk::gl {

// The mine's wall tile set, read from assets/textures/ at startup.
//
// These used to be generated in C++ - render/procedural.cpp, now gone - which
// meant a new tile was a code change and a rebuild. They are ordinary PNGs now,
// with the same standing as the WAVs in assets/sounds/: the manifest is the
// contract, not the contents, and any tile can be repainted or replaced without
// touching this file. tools/make-textures.py still writes the shipped set, so
// the art is reproducible rather than being opaque binaries nobody can rebuild.

// Which layer the engine's own surfaces use.
//
// A level author picks a wall's tile by index, but four surfaces are not
// authored at all - the renderer decides them - so those four are looked up by
// name in the manifest and are fatal if missing. Everything else in the set is
// the author's to arrange.
struct WallTileIndex {
    int rock = 0;      // `pillar` props
    int panel = 0;     // robots, and every prop that is not a pillar
    int hazard = 0;    // door slabs
    int hull = 0;      // the ship, so one texture array covers everything lit
};

// The most layers the emissive pass can carry animation for. Its shader holds
// one vec4 per tile in a uniform array, which has to have a fixed length, and a
// set is rejected at load rather than silently losing the tail.
inline constexpr int kMaxWallTiles = 32;

// How a tile moves, for the tiles that move at all. Read from the manifest and
// applied only in the emissive pass: the lit pass runs once per light, so
// anything computed there is computed again for every light that reaches the
// wall, and a surface bright enough to be worth animating is being drawn by the
// emissive pass anyway.
struct TileAnim {
    // UV units per second. A tile is 5.5 world units, so 0.05 here is a little
    // over a quarter of a unit a second.
    float scrollU = 0.0f;
    float scrollV = 0.0f;
    // Amplitude, in UV, of a sine warp of the lookup - the old trick for
    // flowing lava. Its period is exactly one tile, so the warp meets itself
    // where the tile repeats and animating cannot introduce a seam.
    float turbulence = 0.0f;

    bool still() const { return scrollU == 0.0f && scrollV == 0.0f && turbulence == 0.0f; }
};

struct WallTiles {
    // The square edge the whole set was resampled to, from the manifest. Every
    // layer of a texture array shares one size, so this is set-wide rather than
    // per tile, and raising it is a manifest edit rather than a code change.
    int size = 0;
    WallTileIndex index;
    // Layer order, which is the order the manifest lists and the order a level
    // file's `texture: N` counts in.
    std::vector<std::string> names;
    std::vector<TileAnim> anim;                 // one per layer, still by default
    std::vector<std::vector<uint8_t>> layers;   // size * size * 4 bytes each

    // The layer a name selects, or -1. For callers that want to fail softly;
    // the four in WallTileIndex are already resolved and never need this.
    int indexOf(std::string_view name) const;
};

// Reads the manifest and every tile it names. Fatal on anything wrong with the
// set - a missing file, a tile that is not square, one smaller than the
// manifest's size, a duplicate name, or one of the four required names absent -
// because a wall texture that silently fell back to something else is a bug you
// find by noticing the mine looks odd.
WallTiles loadWallTiles(const char* manifestPath);

// What --validate-assets measures about one tile. Both are advisory: a tile may
// be legitimately dark or legitimately hard-edged, so they are reported as
// warnings and never fail the run.
struct TileReport {
    // Mean luminance over the whole tile, 0-255, weighted the way an eye reads
    // it. Worth knowing because there is no ambient term in the renderer:
    // albedo is the only thing keeping a lit surface off black, so art authored
    // to look dark in an image editor renders as nothing at all.
    float meanLuminance = 0.0f;

    // How harsh the wrap seam is, measured against the tile's own worst
    // interior edge rather than against a fixed number.
    //
    // A fixed threshold cannot tell a seam from a design: a plated tile with a
    // dark groove down one edge shows a big jump where it repeats, and that
    // jump is the groove doing its job. What gives a seam away is a tile that
    // is smooth everywhere except where it joins itself. At or below 1 the
    // wrap is no harsher than an edge already inside the tile; well above 1
    // there is a line across the wall every 5.5 units.
    float seamRatioX = 0.0f;
    float seamRatioY = 0.0f;

    float worstSeamRatio() const { return seamRatioX > seamRatioY ? seamRatioX : seamRatioY; }
};

// `rgba` is one loaded layer, `size` pixels square.
TileReport checkTile(const std::vector<uint8_t>& rgba, int size);

} // namespace dvk::gl
