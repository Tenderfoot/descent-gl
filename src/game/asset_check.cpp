#include "game/asset_check.h"

#include "core/log.h"
#include "game/level.h"
#include "render/model.h"
#include "render/shadow.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace dvk {

namespace {

std::vector<std::string> filesWithExtension(const char* directory, const char* extension) {
    std::vector<std::string> found;
    std::error_code ec;
    const std::filesystem::path dir(directory);
    if (!std::filesystem::is_directory(dir, ec)) return found;

    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() != extension) continue;
        found.push_back(entry.path().generic_string());
    }
    // directory_iterator order is unspecified, and a report whose line order
    // changes between machines is a report nobody can diff.
    std::sort(found.begin(), found.end());
    return found;
}

} // namespace

int validateAssets() {
    int failures = 0;

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
        for (const Segment& seg : level.segments) {
            for (const SideData& side : seg.sides) {
                if (side.isPortal()) ++portals;
            }
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

    log::info("asset validation: {} failure(s)", failures);
    return failures;
}

} // namespace dvk
