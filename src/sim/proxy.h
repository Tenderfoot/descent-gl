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
    enum class Kind : int { Sphere = 0, Box, TaperedCylinder };

    Kind kind = Kind::Sphere;
    // The half extent along each of the model's own axes - for every kind, the
    // box that contains the solid. Sphere: the radius, held in all three
    // components so `boundingRadius` is one read either way. TaperedCylinder:
    // the widest radius on x and z, and the half height on y.
    glm::vec3 halfExtents{1.0f};
    // TaperedCylinder only: the radius at the low-y cap and at the high-y cap,
    // about the volume's own Y axis. A column is a solid of revolution and a
    // box around one is mostly air at the corners - see the note on
    // `taperedCylinderProxy` below.
    glm::vec2 radii{1.0f, 1.0f};
    // The volume's centre in the model's own space. An OBJ sits wherever its
    // author put it, and `pillar.obj` is modelled about its middle while the
    // level places it by that same middle - but nothing guarantees the next
    // model will be, and a column whose origin is its base would otherwise be
    // hit-tested nine units underground.
    glm::vec3 centre{0.0f};

    float boundingRadius() const {
        switch (kind) {
            case Kind::Sphere: return halfExtents.x;
            // The rim of the wider cap, which is as far from the centre as a
            // frustum reaches. Not the box's corner: that would hand every
            // broad-phase reject a radius the solid never fills.
            case Kind::TaperedCylinder:
                return std::sqrt(halfExtents.x * halfExtents.x + halfExtents.y * halfExtents.y);
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

// A conical frustum about the model's own Y axis: `lowRadius` at the bottom
// cap, `highRadius` at the top, `halfHeight` either way from `centre`.
//
// The kind a column wants. `pillar.obj` is a tapered octagon 4.4 across at the
// base and 3.4 at the top, and its bounding box holds 79% more volume than the
// column does: a ship could come to rest 1.4 units clear of anything visible,
// and scaling the entity in the editor multiplied that gap by the scale. The
// radius here is the model's CIRCUMradius - the ring's vertices, not its flats
// - so the solid still contains the mesh everywhere and the error is the
// octagon's own 7.6%, in the direction of stopping at something the player can
// plainly see rather than letting them through it.
inline ProxyVolume taperedCylinderProxy(float lowRadius, float highRadius, float halfHeight,
                                        const glm::vec3& centre = glm::vec3(0.0f)) {
    ProxyVolume v;
    v.kind = ProxyVolume::Kind::TaperedCylinder;
    v.radii = glm::max(glm::vec2(lowRadius, highRadius), glm::vec2(0.0f));
    v.halfExtents = glm::vec3(std::max(v.radii.x, v.radii.y), std::max(halfHeight, 0.0f),
                              std::max(v.radii.x, v.radii.y));
    v.centre = centre;
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
    glm::vec2 radii{1.0f, 1.0f};                // scaled, TaperedCylinder only

    float boundingRadius() const {
        switch (kind) {
            case ProxyVolume::Kind::Sphere: return halfExtents.x;
            case ProxyVolume::Kind::TaperedCylinder:
                return std::sqrt(halfExtents.x * halfExtents.x + halfExtents.y * halfExtents.y);
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
    placed.radii = volume.radii * scale;
    placed.centre = position + orientation * (volume.centre * scale);
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
    const glm::vec3 local = inverse * (p - proxy.centre);
    const glm::vec3 on = proxy.kind == ProxyVolume::Kind::TaperedCylinder
                             ? closestPointOnTaperedCylinder(local, proxy.radii,
                                                             proxy.halfExtents.y)
                             : closestPointOnBox(local, proxy.halfExtents);
    return proxy.centre + proxy.orientation * on;
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

    const bool tapered = proxy.kind == ProxyVolume::Kind::TaperedCylinder;
    const glm::quat inverse = glm::conjugate(proxy.orientation);
    const glm::vec3 local = inverse * (centre - proxy.centre);
    const glm::vec3 nearest =
        tapered ? closestPointOnTaperedCylinder(local, proxy.radii, proxy.halfExtents.y)
                : closestPointOnBox(local, proxy.halfExtents);
    const glm::vec3 away = local - nearest;
    const float distance = glm::length(away);

    glm::vec3 localNormal;
    float depth;
    if (distance > 1e-6f) {
        if (distance >= radius) return false;
        localNormal = away / distance;
        depth = radius - distance;
    } else if (tapered) {
        // Inside the frustum, so the nearest point came back as the point
        // itself and carries no direction. Same rule as the box below - leave
        // through the nearest face - over the two caps and the slanted side.
        const float h = proxy.halfExtents.y;
        float least = local.y + h;                  // out through the bottom
        localNormal = glm::vec3(0.0f, -1.0f, 0.0f);
        if (h - local.y < least) {                  // out through the top
            least = h - local.y;
            localNormal = glm::vec3(0.0f, 1.0f, 0.0f);
        }
        // Out through the side, measured along the side's own normal rather
        // than straight out from the axis: on a taper those differ, and it is
        // the perpendicular distance that says which face is really nearest.
        const glm::vec2 edge(proxy.radii.y - proxy.radii.x, 2.0f * h);
        const float edgeLength = glm::length(edge);
        const float flat = std::sqrt(local.x * local.x + local.z * local.z);
        if (edgeLength > 1e-6f) {
            const glm::vec2 outward(edge.y / edgeLength, -edge.x / edgeLength);
            const float room =
                -((flat - proxy.radii.x) * outward.x + (local.y + h) * outward.y);
            if (room < least) {
                least = room;
                // On the axis no radial direction is more correct than any
                // other, the same as a sphere's concentric case.
                const glm::vec2 radial = flat > 1e-6f ? glm::vec2(local.x, local.z) / flat
                                                      : glm::vec2(1.0f, 0.0f);
                localNormal = glm::vec3(radial.x * outward.x, outward.y, radial.y * outward.x);
            }
        }
        depth = radius + least;
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
        depth = radius + least;
    }

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
    const glm::vec3 localOrigin = inverse * (origin - proxy.centre);
    const glm::vec3 localDir = inverse * dir;
    if (proxy.kind == ProxyVolume::Kind::TaperedCylinder) {
        // Growing a frustum by the projectile's radius moves the caps out by
        // `inflate` and the slanted side out by `inflate` ALONG ITS OWN NORMAL,
        // which measured radially - the only way a radius can be grown here -
        // is more than that. A gentle taper makes the difference tiny and a
        // steep one makes it matter, so it is computed rather than ignored.
        const float h = proxy.halfExtents.y;
        const float rise = proxy.radii.y - proxy.radii.x;
        const float radial =
            h > 0.0f ? inflate * std::sqrt(4.0f * h * h + rise * rise) / (2.0f * h) : inflate;
        return rayTaperedCylinderDistance(localOrigin, localDir, proxy.radii + glm::vec2(radial),
                                          h + inflate);
    }
    return rayBoxDistance(localOrigin, localDir, proxy.halfExtents + glm::vec3(inflate));
}

} // namespace dvk::sim
