#pragma once

#include "sim/geometry.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>

namespace dvk::sim {

// The shape the simulation gives to something that is not a wall.
//
// The mine is triangles and the ships are spheres. Between them sits everything
// else - a column, a crate, a door slab - which is neither, and which until now
// had no shape at all: the combat trace tested every level object as one
// hardcoded 2.4 unit sphere, so a shot at any part of an 18 unit column except
// its middle passed straight through and hit the wall behind.
//
// The answer is not the mesh. A pillar is 28 triangles and the renderer already
// extrudes every one of them into a stencil volume, but nobody can see an
// octagon's facets at the speed a ship crosses a room, and per-triangle queries
// in the sim step are how a projectile system finds its bugs. One volume per
// model, derived from the model's own bounds at load, is enough for every
// question the simulation asks - and it is the SAME volume for all of them, so
// what stops a shot, what stops a ship and what casts into the light query can
// never disagree.
//
// Header-only and allocation-free: this runs per object in the combat trace.
struct ProxyVolume {
    enum class Kind : int { Sphere = 0, Box, ConvexHull };

    Kind kind = Kind::Sphere;
    // The half extent along each of the model's own axes - for every kind, the
    // box that contains the solid. Sphere: the radius, held in all three
    // components so `boundingRadius` is one read either way.
    glm::vec3 halfExtents{1.0f};
    // The volume's centre in the model's own space. An OBJ sits wherever its
    // author put it, and `pillar.obj` is modelled about its middle while the
    // level places it by that same middle - but nothing guarantees the next
    // model will be, and a column whose origin is its base would otherwise be
    // hit-tested nine units underground.
    glm::vec3 centre{0.0f};
    // ConvexHull only: the model's own faces, expressed about `centre` so the
    // transform into it is the same one every other kind uses. The arrays it
    // points at are owned by the level.
    ConvexHullView hull;
    // ConvexHull only: the furthest corner from `centre`.
    float radius = 0.0f;

    float boundingRadius() const {
        switch (kind) {
            case Kind::Sphere: return halfExtents.x;
            // The furthest corner the solid actually has, which for a hull is
            // inside its bounding box. Handing the broad phase the box's corner
            // instead would reject nothing it does not have to.
            case Kind::ConvexHull: return radius;
            default: return glm::length(halfExtents);
        }
    }
};

// A box proxy from an axis-aligned bounds, which is what a model gives you.
inline ProxyVolume boxProxy(const glm::vec3& lo, const glm::vec3& hi) {
    ProxyVolume v;
    v.kind = ProxyVolume::Kind::Box;
    v.halfExtents = glm::max((hi - lo) * 0.5f, glm::vec3(0.0f));
    v.centre = (lo + hi) * 0.5f;
    return v;
}

// The model itself, when the model is convex.
//
// The shape a column wants, and every other convex prop with it. `pillar.obj`
// is a tapered octagon and its bounding box holds 78% more volume than the
// column does: a ship could come to rest 1.4 units clear of anything visible,
// and because `placeProxy` folds the entity's scale into the collider, scaling
// a pillar in the editor multiplied that gap by the scale.
//
// A hull is not an approximation of the mesh, it IS the mesh - for a convex
// model the faces the renderer draws and the planes that bound the solid are
// the same planes. So the divergence this exists to close is not reduced, it
// is zero, and the mesh becomes its own oracle: `rayTriangleDistance` and
// `closestPointOnTriangle` over the model's triangles are exactly what the
// tests compare against.
//
// The arrays behind `hull` are not copied and must outlive every proxy built
// from this volume. The level owns them, and level objects do not move.
inline ProxyVolume convexHullProxy(const ConvexHullView& hull, const glm::vec3& lo,
                                   const glm::vec3& hi, float radius) {
    ProxyVolume v;
    v.kind = ProxyVolume::Kind::ConvexHull;
    v.halfExtents = glm::max((hi - lo) * 0.5f, glm::vec3(0.0f));
    v.centre = (lo + hi) * 0.5f;
    v.hull = hull;
    v.radius = std::max(radius, 0.0f);
    return v;
}

inline ProxyVolume sphereProxy(float radius, const glm::vec3& centre = glm::vec3(0.0f)) {
    ProxyVolume v;
    v.kind = ProxyVolume::Kind::Sphere;
    v.halfExtents = glm::vec3(std::max(radius, 0.0f));
    v.centre = centre;
    return v;
}

// A ProxyVolume put somewhere, with the scale folded in.
//
// Level objects do not move - `Level::objects` is authoring data and assumes
// nothing ever leaves the position it was placed at - so this is built once at
// load. Anything that does move rebuilds it from its own transform.
struct PlacedProxy {
    ProxyVolume::Kind kind = ProxyVolume::Kind::Sphere;
    glm::vec3 centre{0.0f};                     // world space
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 halfExtents{1.0f};                // scaled
    // ConvexHull only. The planes and corners are NOT scaled - one model's hull
    // is shared by every entity placed from it, at whatever scale each was
    // given - so a query divides into the hull's own space by `scale` and
    // multiplies the answer back out. Scale is uniform, so that leaves
    // directions alone and costs one divide.
    ConvexHullView hull;
    float scale = 1.0f;
    float radius = 0.0f;                        // scaled

    float boundingRadius() const {
        switch (kind) {
            case ProxyVolume::Kind::Sphere: return halfExtents.x;
            case ProxyVolume::Kind::ConvexHull: return radius;
            default: return glm::length(halfExtents);
        }
    }
};

// A column: tall enough that its height is the dimension that matters. The
// headless tests pick the level's pillar out by shape rather than by name, so a
// second column under another name is covered too and a level without one
// reports SKIPPED instead of quietly passing.
inline bool proxyIsColumn(const PlacedProxy& proxy) {
    return proxy.kind != ProxyVolume::Kind::Sphere &&
           proxy.halfExtents.y > 2.0f * std::max(proxy.halfExtents.x, proxy.halfExtents.z);
}

inline PlacedProxy placeProxy(const ProxyVolume& volume, const glm::vec3& position,
                              const glm::quat& orientation, float scale) {
    PlacedProxy placed;
    placed.kind = volume.kind;
    placed.orientation = orientation;
    placed.halfExtents = volume.halfExtents * scale;
    placed.centre = position + orientation * (volume.centre * scale);
    placed.hull = volume.hull;
    placed.scale = std::max(scale, 1e-6f);
    placed.radius = volume.radius * scale;
    return placed;
}

// Closest point on the solid to `p`, in world space. A point inside the volume
// comes back unchanged, which is how a caller tells contact from penetration.
inline glm::vec3 closestPointOnProxy(const PlacedProxy& proxy, const glm::vec3& p) {
    if (proxy.kind == ProxyVolume::Kind::Sphere) {
        const glm::vec3 away = p - proxy.centre;
        const float distance = glm::length(away);
        if (distance <= proxy.halfExtents.x || distance < 1e-6f) return p;
        return proxy.centre + away * (proxy.halfExtents.x / distance);
    }
    const glm::quat inverse = glm::conjugate(proxy.orientation);
    if (proxy.kind == ProxyVolume::Kind::ConvexHull && proxy.hull.valid()) {
        const glm::vec3 local = (inverse * (p - proxy.centre)) / proxy.scale;
        const glm::vec3 on = closestPointOnConvexHull(proxy.hull, local);
        return proxy.centre + proxy.orientation * (on * proxy.scale);
    }
    const glm::vec3 local = inverse * (p - proxy.centre);
    return proxy.centre + proxy.orientation * closestPointOnBox(local, proxy.halfExtents);
}

// Where a sphere touches a proxy, and how to get it out.
//
// `closestPointOnProxy` above answers the ray question and returns an interior
// point unchanged, which is the right answer for "what is the nearest point of
// this solid" and the WRONG one for a push-out: a body whose centre has ended
// up inside a box gets a zero-length push direction and stays there. So the
// penetration case is handled explicitly here - the nearest face wins, and the
// body leaves through it.
struct ProxyContact {
    glm::vec3 point{0.0f};    // on the proxy's surface
    glm::vec3 normal{0.0f};   // out of the proxy, towards the sphere
    float depth = 0.0f;       // how far the sphere has to move to clear it
};

// True if a sphere at `centre` with `radius` overlaps the proxy.
inline bool proxySphereContact(const PlacedProxy& proxy, const glm::vec3& centre, float radius,
                               ProxyContact& out) {
    if (proxy.kind == ProxyVolume::Kind::Sphere) {
        const glm::vec3 away = centre - proxy.centre;
        const float distance = glm::length(away);
        const float sum = proxy.halfExtents.x + radius;
        if (distance >= sum) return false;
        // Concentric: no direction is more correct than any other, so take one
        // and let the caller's next iteration sort out the rest.
        out.normal = distance > 1e-6f ? away / distance : glm::vec3(0.0f, 1.0f, 0.0f);
        out.point = proxy.centre + out.normal * proxy.halfExtents.x;
        out.depth = sum - distance;
        return true;
    }

    // A hull's data is unscaled, so the whole question moves into its own
    // space: the sphere shrinks by the same factor the solid would have grown
    // by, and the depth comes back out multiplied. Uniform scale, so the
    // normal needs no correction.
    const bool hulled = proxy.kind == ProxyVolume::Kind::ConvexHull && proxy.hull.valid();
    const float unit = hulled ? proxy.scale : 1.0f;
    const glm::quat inverse = glm::conjugate(proxy.orientation);
    const glm::vec3 local = (inverse * (centre - proxy.centre)) / unit;
    const float localRadius = radius / unit;
    const glm::vec3 nearest = hulled ? closestPointOnConvexHull(proxy.hull, local)
                                     : closestPointOnBox(local, proxy.halfExtents);
    const glm::vec3 away = local - nearest;
    const float distance = glm::length(away);

    glm::vec3 localNormal;
    float depth;
    if (distance > 1e-6f) {
        if (distance >= localRadius) return false;
        localNormal = away / distance;
        depth = localRadius - distance;
    } else if (hulled) {
        // Inside, so the nearest point came back as the point itself and
        // carries no direction. The way out of a convex solid is its nearest
        // face, and for an interior point that is simply the least of the plane
        // distances - no search, and nothing to get wrong at an edge.
        depth = localRadius + convexHullEscape(proxy.hull, local, localNormal);
    } else {
        // Inside the box. The clamp returned the point itself, so there is no
        // direction in it - the way out is the nearest face, which is the axis
        // with the least room left on it.
        int axis = 0;
        float least = proxy.halfExtents[0] - std::abs(local[0]);
        for (int i = 1; i < 3; ++i) {
            const float room = proxy.halfExtents[i] - std::abs(local[i]);
            if (room < least) {
                least = room;
                axis = i;
            }
        }
        localNormal = glm::vec3(0.0f);
        localNormal[axis] = local[axis] >= 0.0f ? 1.0f : -1.0f;
        depth = localRadius + least;
    }
    depth *= unit;

    out.normal = proxy.orientation * localNormal;
    out.point = centre - out.normal * (radius - depth);
    out.depth = depth;
    return true;
}

// Distance along a unit `dir` at which the ray enters the proxy, or a negative
// number if it misses. An origin already inside returns 0.
//
// `inflate` grows the volume by a radius, which is how a projectile with a
// thickness of its own is tested. For a sphere that is exact. For a box it is
// the box grown on every axis, where the true swept shape is a box with rounded
// corners - so a shot passing within `inflate` of a corner counts as a hit when
// strictly it grazed past. That errs towards the shot stopping at something the
// player can plainly see, which is the direction to be wrong in; the alternative
// is a bolt that visibly clips a column and carries on.
inline float rayProxyDistance(const PlacedProxy& proxy, const glm::vec3& origin,
                              const glm::vec3& dir, float inflate = 0.0f) {
    if (proxy.kind == ProxyVolume::Kind::Sphere) {
        const float radius = proxy.halfExtents.x + inflate;
        const glm::vec3 fromCentre = origin - proxy.centre;
        const float projected = glm::dot(fromCentre, dir);
        const float outside = glm::dot(fromCentre, fromCentre) - radius * radius;
        if (outside > 0.0f && projected > 0.0f) return -1.0f;
        const float discriminant = projected * projected - outside;
        if (discriminant < 0.0f) return -1.0f;
        return std::max(0.0f, -projected - std::sqrt(discriminant));
    }
    const glm::quat inverse = glm::conjugate(proxy.orientation);
    if (proxy.kind == ProxyVolume::Kind::ConvexHull && proxy.hull.valid()) {
        // Into the hull's own space, where the ray's parameter is divided by
        // the scale along with everything else, so the answer multiplies back.
        const float unit = proxy.scale;
        const float along = rayConvexHullDistance(proxy.hull,
                                                  (inverse * (origin - proxy.centre)) / unit,
                                                  inverse * dir, inflate / unit);
        return along < 0.0f ? -1.0f : along * unit;
    }
    return rayBoxDistance(inverse * (origin - proxy.centre), inverse * dir,
                          proxy.halfExtents + glm::vec3(inflate));
}

} // namespace dvk::sim
