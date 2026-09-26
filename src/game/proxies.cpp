#include "game/proxies.h"

#include "core/log.h"
#include "game/level.h"
#include "render/model_data.h"
#include "sim/robots.h"

#include <glm/vec3.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace dvk {

namespace {

// What one model resolved to, before the level's flat hull arrays exist.
//
// A hull cannot be finished in the same pass that discovers it: `PlacedProxy`
// holds raw pointers into `Level`'s arrays, and those arrays cannot be grown
// while proxies point into them. So the first pass collects, the level is sized
// once, and the second pass points.
struct Resolved {
    sim::ProxyVolume volume;
    gl::ConvexHullFit hull;    // empty unless `volume.kind` is ConvexHull
    int hullIndex = -1;
};

} // namespace

void resolveObjectProxies(Level& level) {
    level.objectProxies.assign(level.objects.size(), sim::PlacedProxy{});
    level.hullPlanes.clear();
    level.hullVertices.clear();
    level.hullFaceFirst.clear();
    level.hullFaceVertex.clear();

    // Two pillars in one mine are one OBJ read, not two. The cache is local
    // because this runs once per level load and the models are wanted for
    // nothing else afterwards - the renderer keeps its own, because it needs
    // the drawable form and the occluder as well.
    std::unordered_map<std::string, Resolved> byType;
    std::vector<Resolved*> hulled;

    // --- pass one: read every model and decide its shape -------------------
    for (const LevelObject& object : level.objects) {
        // A block has no model to read; its shape is its own `size`.
        if (object.isBlock()) continue;
        if (byType.find(object.type) != byType.end()) continue;

        const std::string path = "assets/models/" + object.type + ".obj";
        const gl::ModelData model = gl::loadObj(path);

        Resolved resolved;
        gl::ConvexHullFit fit;
        glm::vec3 lo(0.0f);
        glm::vec3 hi(0.0f);

        if (gl::modelConvexHull(model, fit)) {
            // The model itself. Nothing is approximated here, so nothing has to
            // be argued about - see `sim::convexHullProxy`.
            resolved.hull = std::move(fit);
            resolved.volume.kind = sim::ProxyVolume::Kind::ConvexHull;
            log::info("proxy '{}': convex hull, {} planes over {} corners, {:.1f} x {:.1f} x "
                      "{:.1f} about ({:.1f} {:.1f} {:.1f})",
                      object.type, resolved.hull.planes.size(), resolved.hull.vertices.size(),
                      resolved.hull.hi.x - resolved.hull.lo.x,
                      resolved.hull.hi.y - resolved.hull.lo.y,
                      resolved.hull.hi.z - resolved.hull.lo.z, resolved.hull.centre.x,
                      resolved.hull.centre.y, resolved.hull.centre.z);
        } else if (gl::modelBounds(model, lo, hi)) {
            resolved.volume = sim::boxProxy(lo, hi);
            // Why it is a box rather than itself, in the log rather than only
            // in the docs: a concave model cannot be its own collider and the
            // number says how far from convex it is.
            log::info("proxy '{}': box {:.1f} x {:.1f} x {:.1f} about ({:.1f} {:.1f} {:.1f}); not "
                      "convex, a corner stands {:.2f}u outside one of its own faces",
                      object.type, resolved.volume.halfExtents.x * 2.0f,
                      resolved.volume.halfExtents.y * 2.0f, resolved.volume.halfExtents.z * 2.0f,
                      resolved.volume.centre.x, resolved.volume.centre.y, resolved.volume.centre.z,
                      fit.worstOutside);
        } else {
            // loadObj is fatal on a file it cannot read, so an empty model
            // here means a valid OBJ with no vertices in it. Nothing to
            // bound, and a zero-size box would silently stop being hit;
            // say so and leave the default sphere.
            log::warn("model '{}' has no vertices; '{}' keeps a unit proxy", path, object.type);
            resolved.volume = sim::sphereProxy(1.0f);
        }

        auto& entry = byType.emplace(object.type, std::move(resolved)).first->second;
        if (entry.volume.kind == sim::ProxyVolume::Kind::ConvexHull) hulled.push_back(&entry);
    }

    // --- pass two: one allocation each, then hand out pointers -------------
    size_t planes = 0;
    size_t vertices = 0;
    size_t faceFirst = 0;
    size_t faceVertex = 0;
    for (const Resolved* entry : hulled) {
        planes += entry->hull.planes.size();
        vertices += entry->hull.vertices.size();
        faceFirst += entry->hull.faceFirst.size();
        faceVertex += entry->hull.faceVertex.size();
    }
    level.hullPlanes.reserve(planes);
    level.hullVertices.reserve(vertices);
    level.hullFaceFirst.reserve(faceFirst);
    level.hullFaceVertex.reserve(faceVertex);

    for (Resolved* entry : hulled) {
        const gl::ConvexHullFit& fit = entry->hull;
        const size_t planeBase = level.hullPlanes.size();
        const size_t vertexBase = level.hullVertices.size();
        const size_t firstBase = level.hullFaceFirst.size();
        const size_t faceBase = level.hullFaceVertex.size();

        level.hullPlanes.insert(level.hullPlanes.end(), fit.planes.begin(), fit.planes.end());
        level.hullVertices.insert(level.hullVertices.end(), fit.vertices.begin(),
                                  fit.vertices.end());
        // The face ranges are rebased, so each hull's view can index from zero.
        for (const int offset : fit.faceFirst) level.hullFaceFirst.push_back(offset);
        for (const int index : fit.faceVertex) level.hullFaceVertex.push_back(index);

        sim::ConvexHullView view;
        view.planes = level.hullPlanes.data() + planeBase;
        view.vertices = level.hullVertices.data() + vertexBase;
        view.faceFirst = level.hullFaceFirst.data() + firstBase;
        view.faceVertex = level.hullFaceVertex.data() + faceBase;
        view.planeCount = static_cast<int>(fit.planes.size());
        view.vertexCount = static_cast<int>(fit.vertices.size());
        entry->volume = sim::convexHullProxy(view, fit.lo, fit.hi, fit.radius);
    }

    for (size_t i = 0; i < level.objects.size(); ++i) {
        const LevelObject& object = level.objects[i];
        if (object.isBlock()) {
            // A box proxy is exact for a box, so this is the drawn shape and
            // not an approximation of it. The scale is already in the size.
            const glm::vec3 half = object.blockSize() * 0.5f;
            level.objectProxies[i] = sim::placeProxy(sim::boxProxy(-half, half), object.position,
                                                     object.orientation, 1.0f);
            continue;
        }
        const auto cached = byType.find(object.type);
        if (cached == byType.end()) continue;
        level.objectProxies[i] = sim::placeProxy(cached->second.volume, object.position,
                                                 object.orientation, object.scale);
    }
}

void resolveRobotAttachments(sim::RobotConfig& robots) {
    for (sim::RobotDef& def : robots.types) {
        if (!def.railgun) continue;
        const std::string path = "assets/models/" + def.railgunModel + ".obj";
        const gl::ModelData model = gl::loadObj(path);
        glm::vec3 lo(0.0f);
        glm::vec3 hi(0.0f);
        if (!gl::modelBounds(model, lo, hi)) {
            log::warn("model '{}' has no vertices; '{}' keeps its configured railgun hitbox", path,
                      def.name);
            continue;
        }
        // A box and not the model's hull: the gun is a receiver with a barrel
        // out of its front, which is not convex, and it is small and moving
        // fast enough that the difference is not something a player can aim at.
        def.railgunVolume = sim::boxProxy(lo, hi);
        log::info("railgun hitbox for '{}': {:.1f} x {:.1f} x {:.1f}", def.name, hi.x - lo.x,
                  hi.y - lo.y, hi.z - lo.z);
    }
}

} // namespace dvk
