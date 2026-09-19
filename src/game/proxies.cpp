#include "game/proxies.h"

#include "core/log.h"
#include "game/level.h"
#include "render/model_data.h"

#include <glm/vec3.hpp>

#include <string>
#include <unordered_map>

namespace dvk {

void resolveObjectProxies(Level& level) {
    level.objectProxies.assign(level.objects.size(), sim::PlacedProxy{});

    // Two pillars in one mine are one OBJ read, not two. The cache is local
    // because this runs once per level load and the models are wanted for
    // nothing else afterwards - the renderer keeps its own, because it needs
    // the drawable form and the occluder as well.
    std::unordered_map<std::string, sim::ProxyVolume> byType;

    for (size_t i = 0; i < level.objects.size(); ++i) {
        const LevelObject& object = level.objects[i];

        auto cached = byType.find(object.type);
        if (cached == byType.end()) {
            const std::string path = "assets/models/" + object.type + ".obj";
            const gl::ModelData model = gl::loadObj(path);

            glm::vec3 lo(0.0f);
            glm::vec3 hi(0.0f);
            gl::TaperedCylinderFit fit;
            sim::ProxyVolume volume;
            if (gl::modelTaperedCylinder(model, fit)) {
                // A column, and a box around one is mostly air at the corners.
                // Circumradius, so the solid still contains every triangle the
                // renderer draws - see `sim::taperedCylinderProxy`.
                volume = sim::taperedCylinderProxy(fit.lowRadius, fit.highRadius, fit.halfHeight,
                                                   fit.centre);
                log::info("proxy '{}': tapered cylinder r {:.2f} to {:.2f} over {:.1f} about "
                          "({:.1f} {:.1f} {:.1f}); {} rings of {}+ sides, {:.0f}% round, profile "
                          "off by {:.3f}u",
                          object.type, fit.lowRadius, fit.highRadius, fit.halfHeight * 2.0f,
                          fit.centre.x, fit.centre.y, fit.centre.z, fit.rings, fit.sides,
                          fit.roundness * 100.0f, fit.profileError);
            } else if (gl::modelBounds(model, lo, hi)) {
                volume = sim::boxProxy(lo, hi);
                log::info("proxy '{}': box {:.1f} x {:.1f} x {:.1f} about ({:.1f} {:.1f} {:.1f})",
                          object.type, volume.halfExtents.x * 2.0f, volume.halfExtents.y * 2.0f,
                          volume.halfExtents.z * 2.0f, volume.centre.x, volume.centre.y,
                          volume.centre.z);
            } else {
                // loadObj is fatal on a file it cannot read, so an empty model
                // here means a valid OBJ with no vertices in it. Nothing to
                // bound, and a zero-size box would silently stop being hit;
                // say so and leave the default sphere.
                log::warn("model '{}' has no vertices; '{}' keeps a unit proxy", path,
                          object.type);
                volume = sim::sphereProxy(1.0f);
            }
            cached = byType.emplace(object.type, volume).first;
        }

        level.objectProxies[i] =
            sim::placeProxy(cached->second, object.position, object.orientation, object.scale);
    }
}

} // namespace dvk
