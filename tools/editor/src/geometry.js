// Derived geometry, ported from splitSides() in src/game/level.cpp.
//
// The loader computes all of this at load time and never stores it in the
// file. The editor recomputes it the same way so that what is on screen is
// what the game will build — in particular the triangulation, which every
// later system in the game uses and which is not a free choice.
//
// Deliberately free of any three.js import: the conformance tests in
// tools/editor/tests run this under node with no browser and no renderer.

import { SIDE_COUNT, SIDE_VERTICES, SPLIT_A, SPLIT_B, WORLD_UNITS_PER_TILE } from './format.js';

export const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
export const add = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
export const scale = (a, s) => [a[0] * s, a[1] * s, a[2] * s];
export const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
export const cross = (a, b) => [
    a[1] * b[2] - a[2] * b[1],
    a[2] * b[0] - a[0] * b[2],
    a[0] * b[1] - a[1] * b[0],
];
export const length = (a) => Math.sqrt(dot(a, a));
export const distance = (a, b) => length(sub(a, b));

export function normalize(a) {
    const len = length(a);
    return len > 0 ? scale(a, 1 / len) : [0, 0, 0];
}

// Matches faceNormal(): a degenerate triangle yields the zero vector rather
// than NaN, because a level under construction has degenerate faces in it and
// the editor must keep drawing.
export function faceNormal(a, b, c) {
    return normalize(cross(sub(b, a), sub(c, a)));
}

// The four corners of a side, in winding order. Level::sideCorners.
//
// A vertex index the level does not have yields the origin instead of
// throwing. The game is fatal on that, and rightly so, but the editor has to
// keep drawing a level that is momentarily broken in order to show where it is
// broken - validate.js reports it as a vertex-range error.
export function sideCorners(level, segmentIndex, side) {
    const seg = level.segments[segmentIndex];
    return SIDE_VERTICES[side].map((corner) => level.vertices[seg.verts[corner]] ?? ORIGIN);
}

const ORIGIN = [0, 0, 0];

// Picks the diagonal that leaves the smaller crease between the two triangles.
//
// Sides are not guaranteed planar - a deformed cube is the whole point of the
// format - and the two diagonals of a warped quad are not equivalent. The one
// with the larger dot product between its triangle normals is the flatter
// fold. preferAltDiagonal() in src/game/level.cpp.
export function preferAltDiagonal(c) {
    const flatA = dot(faceNormal(c[SPLIT_A[0][0]], c[SPLIT_A[0][1]], c[SPLIT_A[0][2]]),
                      faceNormal(c[SPLIT_A[1][0]], c[SPLIT_A[1][1]], c[SPLIT_A[1][2]]));
    const flatB = dot(faceNormal(c[SPLIT_B[0][0]], c[SPLIT_B[0][1]], c[SPLIT_B[0][2]]),
                      faceNormal(c[SPLIT_B[1][0]], c[SPLIT_B[1][1]], c[SPLIT_B[1][2]]));
    return flatB > flatA;
}

// Unauthored UVs get a planar mapping at a fixed world size per tile, so texel
// density is the same on a corridor wall and on a room's raised ceiling.
export function defaultUv(c) {
    const u = distance(c[1], c[0]) / WORLD_UNITS_PER_TILE;
    const v = distance(c[3], c[0]) / WORLD_UNITS_PER_TILE;
    const u2 = distance(c[2], c[3]) / WORLD_UNITS_PER_TILE;
    const v2 = distance(c[2], c[1]) / WORLD_UNITS_PER_TILE;
    return [[0, 0], [u, 0], [u2, v2], [0, v]];
}

// Fills in every field the loader derives: segment centres, and per side the
// chosen diagonal, centre, inward normal and plane constant. Then collects the
// wall triangles, grouped by segment the way Level::walls is.
//
// Call after anything moves a vertex or changes a portal.
export function rebuildDerived(level) {
    const walls = [];
    const wallFirst = [];
    const wallCount = [];

    for (let si = 0; si < level.segments.length; ++si) {
        const seg = level.segments[si];

        let center = [0, 0, 0];
        for (const v of seg.verts) center = add(center, level.vertices[v] ?? ORIGIN);
        seg.center = scale(center, 1 / 8);

        wallFirst.push(walls.length);

        for (let side = 0; side < SIDE_COUNT; ++side) {
            const sd = seg.sides[side];
            const c = sideCorners(level, si, side);

            sd.altDiagonal = preferAltDiagonal(c);
            sd.center = scale(add(add(c[0], c[1]), add(c[2], c[3])), 0.25);

            // An authored `uv` in the file wins; otherwise the loader's planar
            // mapping. Kept on the side so the editor can show texel density.
            sd.effectiveUv = sd.uv ? sd.uv : defaultUv(c);

            const split = sd.altDiagonal ? SPLIT_B : SPLIT_A;
            const n0 = faceNormal(c[split[0][0]], c[split[0][1]], c[split[0][2]]);
            const n1 = faceNormal(c[split[1][0]], c[split[1][1]], c[split[1][2]]);
            sd.normal = normalize(add(n0, n1));
            sd.d = -dot(sd.normal, sd.center);

            if (sd.neighbour >= 0) continue;

            for (let t = 0; t < 2; ++t) {
                const p = [];
                const vi = [];
                for (let k = 0; k < 3; ++k) {
                    const corner = split[t][k];
                    p.push(c[corner]);
                    vi.push(seg.verts[SIDE_VERTICES[side][corner]]);
                }
                walls.push({ p, vi, normal: faceNormal(p[0], p[1], p[2]), segment: si, side });
            }
        }

        wallCount.push(walls.length - wallFirst[si]);
    }

    level.walls = walls;
    level.wallFirst = wallFirst;
    level.wallCount = wallCount;
    return level;
}

// --- portals ----------------------------------------------------------------

// The reciprocal of a portal: which side of the neighbour looks back at `si`,
// or -1 if the graph is one-sided, which validate.js reports as an error.
export function oppositeSide(level, si, side) {
    const neighbour = level.segments[si]?.sides[side]?.neighbour ?? -1;
    const other = level.segments[neighbour];
    if (!other) return -1;
    for (let os = 0; os < SIDE_COUNT; ++os) {
        if (other.sides[os].neighbour === si) return os;
    }
    return -1;
}

// Every portal once, from the lower-numbered segment, carrying both of its
// ends.
//
// A portal has no wall triangles - rebuildDerived skips any side with a
// neighbour, because the game does - so this is the only description of one
// there is. It is what the viewport draws the translucent quad from and what a
// click on that quad resolves against.
//
// Both ends are carried because a portal is a side of two segments, and which
// of them a click means depends on where it was made from; see portalFacing().
export function portalFaces(level) {
    const out = [];
    for (let si = 0; si < level.segments.length; ++si) {
        for (let side = 0; side < SIDE_COUNT; ++side) {
            const sd = level.segments[si].sides[side];
            if (sd.neighbour < 0 || sd.neighbour < si) continue;
            out.push({
                segment: si,
                side,
                neighbour: sd.neighbour,
                neighbourSide: oppositeSide(level, si, side),
                door: sd.door,
                grate: sd.grate ?? -1,
                corners: sideCorners(level, si, side),
            });
        }
    }
    return out;
}

// Which end of a portal a look direction means: the one it is being seen from.
//
// The corners are wound counter-clockwise seen from inside the owning segment,
// so their face normal points into it - the same convention as sd.normal - and
// a direction running against that normal is one arriving from inside. Falling
// back to the owning end on a one-sided portal keeps a click on a broken level
// selecting something rather than nothing.
//
// A door does not care which end it is placed from, because rewireDoors() wires
// both. A selection does: clicking a portal should name the room it was clicked
// from, not whichever of the two happens to be numbered lower.
export function portalFacing(face, direction) {
    // Both triangles, summed, the way rebuildDerived builds sd.normal: a side
    // is not guaranteed planar, and one triangle of a warped quad can point
    // somewhere the side as a whole does not.
    const c = face.corners;
    const inward = add(faceNormal(c[0], c[1], c[2]), faceNormal(c[0], c[2], c[3]));
    if (dot(inward, direction) < 0 || face.neighbourSide < 0) {
        return { segment: face.segment, side: face.side };
    }
    return { segment: face.neighbour, side: face.neighbourSide };
}

// insideSegment() in src/game/level.cpp. Requires rebuildDerived first.
export function insideSegment(level, si, p, slack = 0.01) {
    for (const sd of level.segments[si].sides) {
        if (dot(sd.normal, p) + sd.d < -slack) return false;
    }
    return true;
}

// findSegment(): which segment contains p, or -1. The hint and its neighbours
// are tried first, so "still where it was last step" is a few plane tests.
export function findSegment(level, p, hint = -1) {
    const n = level.segments.length;
    if (hint >= 0 && hint < n && insideSegment(level, hint, p)) return hint;
    if (hint >= 0 && hint < n) {
        for (const sd of level.segments[hint].sides) {
            if (sd.neighbour >= 0 && insideSegment(level, sd.neighbour, p)) return sd.neighbour;
        }
    }
    for (let si = 0; si < n; ++si) {
        if (insideSegment(level, si, p)) return si;
    }
    return -1;
}

// Distance from a point to a triangle, used by the light flood fill to measure
// reach to the nearest point of a portal rather than to a segment centre.
// closestPointOnTriangle / distanceToTriangle in src/sim/geometry.h, including
// its zero-denominator guards: a level under construction has degenerate
// triangles in it, and the editor must return a number rather than NaN.
export function distanceToTriangle(p, a, b, c) {
    const ab = sub(b, a);
    const ac = sub(c, a);
    const ap = sub(p, a);

    const d1 = dot(ab, ap);
    const d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return distance(p, a);           // vertex region A

    const bp = sub(p, b);
    const d3 = dot(ab, bp);
    const d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return distance(p, b);          // vertex region B

    const vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {                     // edge AB
        const denom = d1 - d3;
        return distance(p, add(a, scale(ab, denom !== 0 ? d1 / denom : 0)));
    }

    const cp = sub(p, c);
    const d5 = dot(ab, cp);
    const d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return distance(p, c);          // vertex region C

    const vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {                     // edge AC
        const denom = d2 - d6;
        return distance(p, add(a, scale(ac, denom !== 0 ? d2 / denom : 0)));
    }

    const va = d3 * d6 - d5 * d4;
    if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) {           // edge BC
        const denom = (d4 - d3) + (d5 - d6);
        return distance(p, add(b, scale(sub(c, b), denom !== 0 ? (d4 - d3) / denom : 0)));
    }

    const denom = va + vb + vc;
    if (denom === 0) return distance(p, a);                  // degenerate triangle
    return distance(p, add(a, add(scale(ab, vb / denom), scale(ac, vc / denom))));
}

// rebuildLightSegments(): every segment this light can possibly reach,
// flood-filled from its own and stopped at any portal whose nearest point is
// farther away than the radius. Doors are not considered - the editor shows
// the level-load state, where no door has moved yet.
export function lightSegments(level, light) {
    if (light.segment < 0 || light.segment >= level.segments.length) return [];

    const seen = new Set([light.segment]);
    const queue = [light.segment];
    const out = [];

    for (let head = 0; head < queue.length; ++head) {
        const si = queue[head];
        out.push(si);
        for (let side = 0; side < SIDE_COUNT; ++side) {
            const sd = level.segments[si].sides[side];
            if (sd.neighbour < 0 || seen.has(sd.neighbour)) continue;
            const c = sideCorners(level, si, side);
            const nearest = Math.min(distanceToTriangle(light.position, c[0], c[1], c[2]),
                                     distanceToTriangle(light.position, c[0], c[2], c[3]));
            if (nearest > light.radius) continue;
            seen.add(sd.neighbour);
            queue.push(sd.neighbour);
        }
    }

    return out.sort((a, b) => a - b);
}
