// Operations that change a level's structure.
//
// Every one of these preserves the format's central rule by construction
// rather than by checking afterwards: two segments joined through a portal
// reference THE SAME FOUR VERTEX INDICES. Extrude shares them because it
// builds the new segment out of them; join shares them by rewriting one
// segment's indices to the other's. There is no operation here that creates a
// portal by moving vertices until they coincide, because that is precisely the
// level file that validates, renders, walks - and leaks light.
//
// Free of any three.js import, so tools/editor/tests can run it under node.

import { OPPOSITE_SIDE, SIDE_COUNT, SIDE_NAMES, SIDE_VERTICES } from './format.js';
import { add, distance, rebuildDerived, scale, sub } from './geometry.js';
import { rewireDoors } from './entities.js';

// The cube corner numbering the format is built on, from src/game/level.h:
//   0 (-x,-y,-z)  1 (+x,-y,-z)  2 (+x,+y,-z)  3 (-x,+y,-z)
//   4 (-x,-y,+z)  5 (+x,-y,+z)  6 (+x,+y,+z)  7 (-x,+y,+z)
const CUBE_CORNERS = [
    [-1, -1, -1], [1, -1, -1], [1, 1, -1], [-1, 1, -1],
    [-1, -1, 1], [1, -1, 1], [1, 1, 1], [-1, 1, 1],
];

// Which axis each side faces. left/right x, top/bottom y, back/front z.
const SIDE_AXIS = [0, 1, 0, 1, 2, 2];

// For each side, the permutation of the eight corner slots that reflects the
// cube through that side's plane.
//
// This is the whole of the corner-correspondence problem, and it is derived
// rather than written down. A hand-built table would be right for the side it
// was checked on and quietly twisted for the other five - a twisted segment
// still validates, because every structural rule is about indices and this is
// about which index goes in which slot. tests/ops.mjs extrudes in all six
// directions and checks the result is a box, not just a legal one.
const MIRROR = SIDE_AXIS.map((axis) => CUBE_CORNERS.map((corner) => {
    const target = corner.map((v, k) => (k === axis ? -v : v));
    return CUBE_CORNERS.findIndex((c) => c[0] === target[0] && c[1] === target[1] && c[2] === target[2]);
}));

const EPSILON = 1e-4;   // the same weld tolerance the closure check uses

// --- helpers ---------------------------------------------------------------

function pushVertex(level, position) {
    // Reuse a coincident vertex rather than adding a second one at the same
    // place: two indices for one position is exactly what the closure check
    // reports as an open mesh.
    for (let i = 0; i < level.vertices.length; ++i) {
        if (distance(level.vertices[i], position) < EPSILON) return i;
    }
    level.vertices.push(position.slice());
    return level.vertices.length - 1;
}

const wall = (texture = 0) => ({ neighbour: -1, door: -1, texture, uv: null });

// A side's outward direction. sd.normal points INTO the segment, because the
// mine is a space you are inside.
function outwardNormal(level, si, side) {
    return scale(level.segments[si].sides[side].normal, -1);
}

// How deep a segment is through a given side, measured centre to centre. Used
// as the default extrusion depth so a new segment matches the one it grew from.
export function segmentDepth(level, si, side) {
    const seg = level.segments[si];
    return distance(seg.sides[side].center, seg.sides[OPPOSITE_SIDE[side]].center);
}

// Drops vertices no segment references any more, and renumbers the rest.
// Called by every operation that can orphan one, so the vertex array never
// accumulates debris that a later join would match against.
export function compactVertices(level) {
    const used = new Set();
    for (const seg of level.segments) for (const v of seg.verts) used.add(v);

    const remap = new Array(level.vertices.length).fill(-1);
    const kept = [];
    for (let i = 0; i < level.vertices.length; ++i) {
        if (!used.has(i)) continue;
        remap[i] = kept.length;
        kept.push(level.vertices[i]);
    }
    if (kept.length === level.vertices.length) return 0;

    const removed = level.vertices.length - kept.length;
    level.vertices = kept;
    for (const seg of level.segments) seg.verts = seg.verts.map((v) => remap[v]);
    return removed;
}

// --- extrude ---------------------------------------------------------------

// Grows a new segment out of a wall, and makes the two a portal pair.
//
// The four corners of `side` become four of the new segment's corners - the
// same indices, not copies - and the other four are new vertices pushed out
// along the side's outward normal. That is what makes the portal exact by
// construction.
export function extrudeSide(level, si, side, depth = null) {
    const seg = level.segments[si];
    if (seg.sides[side].neighbour >= 0) {
        throw new Error(`segment ${si} side ${SIDE_NAMES[side]} is already a portal`);
    }

    const outward = outwardNormal(level, si, side);
    const distanceOut = depth ?? segmentDepth(level, si, side);
    const mirror = MIRROR[side];

    const verts = new Array(8).fill(-1);
    for (const corner of SIDE_VERTICES[side]) {
        // The shared face: the new segment's matching slot takes the existing
        // vertex index.
        verts[mirror[corner]] = seg.verts[corner];
        // The far face: a new vertex, pushed out from the one it mirrors.
        verts[corner] = pushVertex(level,
            add(level.vertices[seg.verts[corner]], scale(outward, distanceOut)));
    }

    const created = level.segments.length;
    const sides = [];
    for (let s = 0; s < SIDE_COUNT; ++s) sides.push(wall(seg.sides[side].texture));
    sides[OPPOSITE_SIDE[side]] = { neighbour: si, door: -1, texture: 0, uv: null };

    level.segments.push({ verts, sides, keyOrder: null });
    seg.sides[side] = { neighbour: created, door: -1, texture: 0, uv: null };

    rebuildDerived(level);
    return created;
}

// --- join ------------------------------------------------------------------

// Makes two existing walls into a portal pair.
//
// Their four corners must already be in the same places; this does not move
// geometry. What it does is make them the same *indices*: every reference to
// one segment's four corner vertices is rewritten to the other's, so the two
// segments genuinely share them and the seam cannot leak.
export function joinSides(level, aSeg, aSide, bSeg, bSide, tolerance = 0.05) {
    if (aSeg === bSeg) throw new Error('a segment cannot open into itself');
    for (const [si, side] of [[aSeg, aSide], [bSeg, bSide]]) {
        if (level.segments[si].sides[side].neighbour >= 0) {
            throw new Error(`segment ${si} side ${SIDE_NAMES[side]} is already a portal`);
        }
    }

    const a = level.segments[aSeg];
    const b = level.segments[bSeg];

    // Pair each of A's corners with the nearest of B's, then insist the
    // pairing is a bijection. Nearest-match alone would happily map two of A's
    // corners onto one of B's for a face that is merely close, and the result
    // would be a level that validates and is geometrically nonsense.
    const pairs = new Map();
    for (const cornerA of SIDE_VERTICES[aSide]) {
        const pa = level.vertices[a.verts[cornerA]];
        let best = -1;
        let bestDistance = Infinity;
        for (const cornerB of SIDE_VERTICES[bSide]) {
            const d = distance(pa, level.vertices[b.verts[cornerB]]);
            if (d < bestDistance) { bestDistance = d; best = cornerB; }
        }
        if (bestDistance > tolerance) {
            throw new Error(
                `these sides are not in the same place: a corner of segment ${aSeg} ` +
                `${SIDE_NAMES[aSide]} is ${bestDistance.toFixed(3)} from the nearest corner of ` +
                `segment ${bSeg} ${SIDE_NAMES[bSide]}`);
        }
        pairs.set(cornerA, best);
    }
    if (new Set(pairs.values()).size !== 4) {
        throw new Error('these sides do not line up corner for corner - two of them pair with ' +
                        'the same vertex, so the join would fold the face');
    }

    // Rewrite B's four vertex indices to A's, everywhere in the level. Doing
    // it globally rather than on segment B alone is the point: any other
    // segment already sharing one of B's corners has to follow it.
    const rewrite = new Map();
    for (const [cornerA, cornerB] of pairs) {
        rewrite.set(b.verts[cornerB], a.verts[cornerA]);
    }
    for (const seg of level.segments) {
        seg.verts = seg.verts.map((v) => (rewrite.has(v) ? rewrite.get(v) : v));
    }

    a.sides[aSide] = { neighbour: bSeg, door: -1, texture: 0, uv: null };
    b.sides[bSide] = { neighbour: aSeg, door: -1, texture: 0, uv: null };

    compactVertices(level);
    rebuildDerived(level);
}

// --- delete ----------------------------------------------------------------

// Removes a segment and every reference to it.
//
// Its portals become walls on the far side, doors on it go, and every index
// above it shifts down - in the segment graph, the doors, and each entity's
// cached segment. Lights, pickups and objects that were inside it go with it,
// because deleting a room and keeping the lamp floating in the rock it became
// is not what anyone means. The player start and the exit do NOT: they are
// the level, not furniture, so they are left pointing at nothing and the
// validator says so loudly.
//
// Returns what it removed, so the caller can say.
//
// Note what this does not attempt: the resulting wall surface may not be
// closed. Removing a segment from the middle of a cluster can leave four
// faces meeting along one edge, which is a pinch the shadow volumes cannot
// use. No amount of cleverness here fixes that - the geometry genuinely
// pinches - so the closure check reports it and the author decides.
export function deleteSegment(level, si) {
    if (level.segments.length <= 1) throw new Error('a level needs at least one segment');

    // Seal the neighbours first, while the indices still mean what they say.
    for (let side = 0; side < SIDE_COUNT; ++side) {
        const sd = level.segments[si].sides[side];
        if (sd.neighbour < 0) continue;
        const other = level.segments[sd.neighbour];
        for (let os = 0; os < SIDE_COUNT; ++os) {
            if (other.sides[os].neighbour === si) other.sides[os] = wall(sd.texture);
        }
    }

    level.doors = level.doors.filter((d) => d.segment !== si && d.neighbour !== si &&
                                            level.segments[d.segment]?.sides[d.side]?.neighbour !== si);

    const removed = { lights: 0, pickups: 0, objects: 0 };
    for (const [key, list] of [['lights', 'lights'], ['pickups', 'pickups'], ['objects', 'objects']]) {
        const before = level[list].length;
        level[list] = level[list].filter((entity) => entity.segment !== si);
        removed[key] = before - level[list].length;
    }

    level.segments.splice(si, 1);

    const shift = (index) => (index === si ? -1 : index > si ? index - 1 : index);
    for (const seg of level.segments) {
        for (let side = 0; side < SIDE_COUNT; ++side) {
            const sd = seg.sides[side];
            if (sd.neighbour >= 0) sd.neighbour = shift(sd.neighbour);
        }
    }
    for (const door of level.doors) door.segment = shift(door.segment);
    for (const list of [level.lights, level.pickups, level.objects]) {
        for (const entity of list) entity.segment = shift(entity.segment);
    }
    if (level.exitTrigger) level.exitTrigger.segment = shift(level.exitTrigger.segment);
    level.start.segment = shift(level.start.segment);

    // Door indices on sides are positions in level.doors, which just moved.
    rewireDoors(level);

    removed.vertices = compactVertices(level);
    rebuildDerived(level);
    return removed;
}

// --- smaller edits ----------------------------------------------------------

// Moves one vertex. Every segment referencing it deforms, which is the whole
// reason the format shares a vertex array: a portal cannot come apart.
export function moveVertex(level, index, position) {
    level.vertices[index] = position.slice();
    rebuildDerived(level);
}

export function setTexture(level, si, side, texture) {
    const sd = level.segments[si].sides[side];
    if (sd.neighbour >= 0) throw new Error('a portal has no texture - it is an opening');
    sd.texture = texture;
}

// Turns a portal back into a pair of walls. The segments keep sharing their
// four vertices, which is harmless; the surface closes because both sides now
// contribute triangles... except they would contribute the SAME triangles
// twice. So this is only offered where the two segments can be pulled apart
// afterwards, and the closure check is what says whether that happened.
export function splitPortal(level, si, side) {
    const sd = level.segments[si].sides[side];
    if (sd.neighbour < 0) throw new Error('that side is already a wall');
    if (sd.door >= 0) throw new Error('remove the door before splitting the portal');

    const other = level.segments[sd.neighbour];
    const texture = 0;
    for (let os = 0; os < SIDE_COUNT; ++os) {
        if (other.sides[os].neighbour === si) other.sides[os] = wall(texture);
    }
    level.segments[si].sides[side] = wall(texture);
    rebuildDerived(level);
}

export { MIRROR, CUBE_CORNERS, SIDE_AXIS };
