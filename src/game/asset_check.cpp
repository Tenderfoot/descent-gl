#include "game/asset_check.h"

#include "core/log.h"
#include "game/level.h"
#include "game/level_list.h"
#include "render/model.h"
#include "render/shadow.h"
#include "render/wall_tiles.h"

#include <string>
#include <vector>

namespace dvk {
namespace {

constexpr const char* kWallTileManifest = "assets/textures/tiles.json";

// Below this mean luminance a tile is almost certainly art that was authored to
// look dark rather than art that is dark on purpose. There is no ambient term
// in the renderer, so such a tile does not read as gloomy, it reads as nothing:
// the shipped set sits between 63 and 156, the painted tiles at the darker end
// of that and the generated ones at the brighter.
constexpr float kMinMeanLuminance = 48.0f;

// Above this, the join where a tile repeats is several times harsher than any
// edge inside it, which is what a seam is. See TileReport.
constexpr float kMaxSeamRatio = 4.0f;

}   // namespace

int validateAssets() {
    int failures = 0;
    int warnings = 0;

    // The tile set first: loadWallTiles is fatal on a set that will not load at
    // all, and the level checks below need the layer count before they can say
    // whether a side's `texture` points at a tile that exists.
    const gl::WallTiles tiles = gl::loadWallTiles(kWallTileManifest);
    for (size_t i = 0; i < tiles.layers.size(); ++i) {
        const gl::TileReport report = gl::checkTile(tiles.layers[i], tiles.size);
        const std::string& name = tiles.names[i];

        // Neither of these fails the run. A tile may be dark or hard-edged
        // because that is what was wanted, and --validate-assets returning
        // non-zero should mean something is broken, not that something is worth
        // a second look.
        if (report.meanLuminance < kMinMeanLuminance) {
            ++warnings;
            log::warn("tile WARN {} (layer {}): mean luminance {:.0f} is below {:.0f}. With no "
                      "ambient term this will read as black wherever a light does not fall "
                      "directly on it.",
                      name, i, report.meanLuminance, kMinMeanLuminance);
        }
        if (report.worstSeamRatio() > kMaxSeamRatio) {
            ++warnings;
            log::warn("tile WARN {} (layer {}): the wrap seam is {:.1f}x the harshest edge inside "
                      "the tile (x {:.1f}, y {:.1f}). A wall shows about four repeats, so this "
                      "will read as a line across it.",
                      name, i, report.worstSeamRatio(), report.seamRatioX, report.seamRatioY);
        }
        log::info("tile ok    {} (layer {}): {}px, mean luminance {:.0f}, seam {:.1f}x", name, i,
                  tiles.size, report.meanLuminance, report.worstSeamRatio());
    }

    for (const std::string& path : filesWithExtension("assets/models", ".obj")) {
        // loadObj is fatal on a missing or unparsable file. That is left alone:
        // a file that is not an OBJ at all is a different problem from a model
        // that is an OBJ and is open, and only the second one is worth
        // continuing past.
        const gl::ModelData model = gl::loadObj(path);
        const gl::ClosureReport report = gl::checkClosure(model.positions, model.triangles);

        if (report.closed()) {
            log::info("asset ok   {}: {} vertices, {} triangles, {} edges, closed", path,
                      report.weldedVertices, report.triangles, report.edges);
            continue;
        }

        ++failures;
        log::error(
            "asset FAIL {}: {} open edge(s), {} edge(s) shared by three or more; first at "
            "({:.3f} {:.3f} {:.3f}) - ({:.3f} {:.3f} {:.3f})",
            path, report.openEdges, report.overSharedEdges, report.badA.x, report.badA.y,
            report.badA.z, report.badB.x, report.badB.y, report.badB.z);
#ifdef DVK_DEBUG
        log::fatal("{} is not a closed mesh. Shadow volumes need every edge to have exactly two "
                   "triangles; this is an asset error, not a renderer one.",
                   path);
#else
        log::error("skipping {} and continuing, because this is a Release build", path);
#endif
    }

    for (const std::string& path : filesWithExtension("assets/levels", ".json")) {
        // A level that fails validation is fatal in both configurations, which
        // is a deliberate departure from "skip it with a warning in Release".
        // A skipped model costs a pillar; a skipped level leaves nothing to
        // play, so "continue without it" is not a better outcome than stopping
        // and saying why. loadLevel already fails this way, and the report here
        // is what makes it one line per asset rather than a silent success.
        const Level level = loadLevel(path);

        const gl::ClosureReport walls = gl::checkClosure(
            std::vector<glm::vec3>(level.vertices.begin(), level.vertices.end()), [&] {
                std::vector<glm::uvec3> tris;
                tris.reserve(level.walls.size());
                for (const WallTriangle& w : level.walls) {
                    tris.push_back({static_cast<unsigned>(w.vi[0]), static_cast<unsigned>(w.vi[1]),
                                    static_cast<unsigned>(w.vi[2])});
                }
                return tris;
            }());

        int portals = 0;
        // A `texture` index that points past the end of the set. Worth checking
        // now that the set is a manifest rather than an enum: shortening the
        // list is a one-line edit, and the layer it removes is referenced by
        // whichever levels were already using it. GL clamps an out-of-range
        // array layer rather than complaining, so without this the only symptom
        // is a wall wearing the last tile in the set.
        int outOfRange = 0;
        const int layers = static_cast<int>(tiles.layers.size());
        for (size_t si = 0; si < level.segments.size(); ++si) {
            const Segment& seg = level.segments[si];
            for (int side = 0; side < SideCount; ++side) {
                const SideData& sd = seg.sides[static_cast<size_t>(side)];
                if (sd.isPortal()) {
                    ++portals;
                    continue;
                }
                if (sd.texture < 0 || sd.texture >= layers) {
                    ++outOfRange;
                    log::error("asset FAIL {}: segment {} side {} uses texture {}, and the set has "
                               "{} tile(s)",
                               path, si, kSideNames[static_cast<size_t>(side)], sd.texture, layers);
                }
            }
        }
        if (outOfRange > 0) {
            ++failures;
            log::fatal("{} references {} tile(s) that are not in {}. Either the level is wrong or "
                       "the manifest lost a layer it was counting on.",
                       path, outOfRange, kWallTileManifest);
        }

        if (walls.closed()) {
            log::info("asset ok   {}: {} segments, {} portal sides, {} wall triangles, surface "
                      "closed (V-E+F = {})",
                      path, level.segmentCount(), portals, static_cast<int>(level.walls.size()),
                      walls.weldedVertices - walls.edges + walls.triangles);
        } else {
            ++failures;
            log::error("asset FAIL {}: wall surface is not closed - {} open edge(s), {} over-shared",
                       path, walls.openEdges, walls.overSharedEdges);
            log::fatal("{} does not form a closed interior surface. The mine's own shadow volume "
                       "is built from it, and an open volume makes depth-fail counting wrong.",
                       path);
        }
    }

    log::info("asset validation: {} failure(s), {} warning(s)", failures, warnings);
    return failures;
}

} // namespace dvk
