#pragma once

#include "sim/proxy.h"

#include <array>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

namespace dvk::sim {

// Everything that stands between a light and a surface, or between an eye and
// its target, and is not the mine.
//
// The renderer has cast shadows from robots, props, the ship and door slabs
// since M3. The simulation never has: `addIllumination` traced wall triangles
// and door faces and nothing else, and the robot sight ray did the same. So a
// robot standing in a beam darkened the picture but not the number, and a
// pillar between a robot and the player blocked neither. `src/sim/lighting.h`
// and docs/ARCHITECTURE.md both described that gap as permanent, on the grounds
// that the occluder meshes live behind `render/gl.h`. Proxy volumes made it
// untrue - `sim` never reads an asset, it is handed finished shapes.
//
// Rebuilt every step and fixed-capacity, because the sim step does not allocate.
enum class OccluderKind : int { None = 0, Ship, Robot, Object, Prop };

// Which thing an occluder IS, so a query can leave it out.
//
// This is not bookkeeping, it is the difference between working and broken.
// `playerVisibility` samples the ship's own centre: include the ship's proxy
// and every shadow ray starts inside it, the player reads unlit against every
// light, and nothing ever notices them again. Same for a light inside its own
// housing - a floating light would put itself out.
struct OccluderId {
    OccluderKind kind = OccluderKind::None;
    int index = -1;

    bool operator==(const OccluderId& other) const {
        return kind == other.kind && index == other.index;
    }
    bool valid() const { return kind != OccluderKind::None; }
};

struct OccluderEntry {
    PlacedProxy proxy;
    OccluderId id;
    // Where it stands. NOT used to cull the ray below, deliberately: a sight
    // line crosses rooms, and an occluder is either geometrically on that line
    // or it is not. Anything the line should not have reached was already
    // stopped by the wall raycast that runs first. Kept because every caller
    // has it to hand and a debug overlay wants it.
    int segment = -1;
};

// The ship, every live robot, every floating light and every solid object at
// once, with room to spare.
inline constexpr int kMaxOccluders = 80;

struct OccluderSet {
    std::array<OccluderEntry, kMaxOccluders> entries{};
    int count = 0;

    void clear() { count = 0; }

    bool add(const PlacedProxy& proxy, OccluderId id, int segment) {
        if (count >= kMaxOccluders) return false;
        OccluderEntry& e = entries[static_cast<size_t>(count++)];
        e.proxy = proxy;
        e.id = id;
        e.segment = segment;
        return true;
    }

    // A ship, a robot or a floating light: a sphere of the body's own radius.
    //
    // Not the model's bounds. The body radius is the authored, tuned number
    // that collision and gunfire already agree on, and a robot that blocks
    // sight over a different extent than it blocks bullets is a robot the
    // player cannot aim at.
    bool addBody(const glm::vec3& position, float radius, OccluderId id, int segment) {
        PlacedProxy proxy;
        proxy.kind = ProxyVolume::Kind::Sphere;
        proxy.centre = position;
        proxy.halfExtents = glm::vec3(radius);
        return add(proxy, id, segment);
    }
};

// Is anything in the set across the segment from `from` to `to`?
//
// `ignoreA` and `ignoreB` are left out: the thing being asked about and the
// thing doing the asking - an eye and its target, or a light and the body it is
// falling on. Both may be default-constructed to ignore nothing.
//
// The ray is clipped to the segment between the two points, so an occluder
// beyond the target does not count. Allocation-free, and cheap enough to run
// per light per sample: the bounding-radius reject below is a dot product and a
// compare, and it throws out everything not roughly on the line.
inline bool occluded(const OccluderSet& set, const glm::vec3& from, const glm::vec3& to,
                     const OccluderId& ignoreA = OccluderId{},
                     const OccluderId& ignoreB = OccluderId{}) {
    const glm::vec3 along = to - from;
    const float distance = glm::length(along);
    if (distance < 1e-4f) return false;
    const glm::vec3 dir = along / distance;

    for (int i = 0; i < set.count; ++i) {
        const OccluderEntry& e = set.entries[static_cast<size_t>(i)];
        if (ignoreA.valid() && e.id == ignoreA) continue;
        if (ignoreB.valid() && e.id == ignoreB) continue;

        // Cheap reject: how far the occluder's centre sits off the line, against
        // the radius of the sphere that contains it.
        const glm::vec3 toCentre = e.proxy.centre - from;
        const float alongRay = glm::dot(toCentre, dir);
        const float bound = e.proxy.boundingRadius();
        if (alongRay < -bound || alongRay > distance + bound) continue;
        const float offLine = glm::length(toCentre - dir * alongRay);
        if (offLine > bound) continue;

        const float hit = rayProxyDistance(e.proxy, from, dir);
        if (hit >= 0.0f && hit < distance) return true;
    }
    return false;
}

} // namespace dvk::sim
