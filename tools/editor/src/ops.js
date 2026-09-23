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
import { rewireDoors, rewireGrates } from './entities.js';

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

const wall = (texture = 0) => ({ neighbour: -1, door: -1, grate: -1, texture, emissive: 0, uv: null });

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
    sides[OPPOSITE_SIDE[side]] = { neighbour: si, door: -1, grate: -1, texture: 0, emissive: 0, uv: null };

    level.segments.push({ verts, sides, keyOrder: null });
    seg.sides[side] = { neighbour: created, door: -1, grate: -1, texture: 0, emissive: 0, uv: null };

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
// src/ops.js

export function joinSides(level, s1, side1, s2, side2, tolerance = 1.0) {
  const seg1 = level.segments[s1];
  const seg2 = level.segments[s2];

  if (seg1.sides[side1].neighbour >= 0 || seg2.sides[side2].neighbour >= 0) {
    throw new Error('one or both sides are already portals');
  }

  // Get corner indices for both sides in winding order
  const corners1 = SIDE_VERTICES[side1].map((c) => seg1.verts[c]);
  const corners2 = SIDE_VERTICES[side2].map((c) => seg2.verts[c]);

  // Find which rotation/alignment of corners2 minimizes total distance to corners1
  let bestShift = 0;
  let bestReverse = false;
  let minTotalDist = Infinity;

  const getPos2 = (idx) => level.vertices[corners2[idx]];

  for (const reverse of [false, true]) {
    for (let shift = 0; shift < 4; ++shift) {
      let totalDist = 0;
      for (let i = 0; i < 4; ++i) {
        const j = reverse ? (4 - i + shift) % 4 : (i + shift) % 4;
        totalDist += distance(level.vertices[corners1[i]], getPos2(j));
      }
      if (totalDist < minTotalDist) {
        minTotalDist = totalDist;
        bestShift = shift;
        bestReverse = reverse;
      }
    }
  }

  // Build the corner mapping from seg1 corners to seg2 corners
  const mappedCorners2 = [0, 1, 2, 3].map((i) => {
    const j = bestReverse ? (4 - i + bestShift) % 4 : (i + bestShift) % 4;
    return corners2[j];
  });

  // Verify each corresponding pair is within the tolerance
  for (let i = 0; i < 4; ++i) {
    const v1 = corners1[i];
    const v2 = mappedCorners2[i];
    const d = distance(level.vertices[v1], level.vertices[v2]);

    if (d > tolerance) {
      throw new Error(`these sides are not in the same place: a corner is ${d.toFixed(3)} units away`);
    }
  }

  // Rewrite segment 2's vertex slots so they point directly to segment 1's vertex indices
  for (let i = 0; i < 4; ++i) {
    const v1 = corners1[i];
    const v2 = mappedCorners2[i];

    // Find where v2 was used in seg2 and point it at v1 instead
    for (let slot = 0; slot < 8; ++slot) {
      if (seg2.verts[slot] === v2) {
        seg2.verts[slot] = v1;
      }
    }
  }

  // Set the reciprocal portals
  seg1.sides[side1].neighbour = s2;
  seg2.sides[side2].neighbour = s1;

  // Clean up unused orphaned vertices and update internal stats
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
    level.grates = level.grates.filter((g) => g.segment !== si &&
                                              level.segments[g.segment]?.sides[g.side]?.neighbour !== si);

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
    for (const grate of level.grates) grate.segment = shift(grate.segment);
    for (const list of [level.lights, level.pickups, level.objects]) {
        for (const entity of list) entity.segment = shift(entity.segment);
    }
    if (level.exitTrigger) level.exitTrigger.segment = shift(level.exitTrigger.segment);
    level.start.segment = shift(level.start.segment);

    // Door and grate indices on sides are positions in their lists, which
    // just moved.
    rewireDoors(level);
    rewireGrates(level);

    removed.vertices = compactVertices(level);
    rebuildDerived(level);
    return removed;
}

// --- smaller edits ----------------------------------------------------------

// Moves one vertex. Every segment referencing it deforms, which is the whole
// reason the format shares a vertex array: a portal cannot come apart.
export function moveVertex(level, index, position) {
    moveVertices(level, [[index, position]]);
}

// Several vertices at once, from a group transform.
//
// One rebuildDerived for the whole group rather than one per vertex: the
// derived planes, centres and diagonals are a function of the FINAL positions,
// so rebuilding per vertex costs N times as much for the same answer - and
// during a drag of a segment's eight corners that is eight full rebuilds a
// frame.
//
// `updates` is a list of [index, position]. Every index is checked before
// anything is written, so a bad one leaves the level untouched, which is what
// edit() in main.js relies on to keep its undo entry honest.
export function moveVertices(level, updates) {
    for (const [index] of updates) {
        if (!Number.isInteger(index) || index < 0 || index >= level.vertices.length) {
            throw new Error(`no vertex ${index}`);
        }
    }
    for (const [index, position] of updates) {
        level.vertices[index] = position.slice();
    }
    rebuildDerived(level);
}

export function setTexture(level, si, side, texture) {
    const sd = level.segments[si].sides[side];
    if (sd.neighbour >= 0) throw new Error('a portal has no texture - it is an opening');
    sd.texture = texture;
}

// How brightly a side emits on top of the light that reaches it. 0 is an
// ordinary wall.
//
// This does not light the room. The renderer has no area lights, so a pool that
// should also lay light on what is around it wants a Light in the segment as
// well - which is why the side panel offers to drop one.
export function setEmissive(level, si, side, emissive) {
    const sd = level.segments[si].sides[side];
    if (sd.neighbour >= 0) throw new Error('a portal has no surface to emit from');
    if (!(emissive >= 0)) throw new Error(`emissive must be zero or more, got ${emissive}`);
    sd.emissive = emissive;
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
    if (sd.grate >= 0) throw new Error('remove the grate before splitting the portal');

    const other = level.segments[sd.neighbour];
    const texture = 0;
    for (let os = 0; os < SIDE_COUNT; ++os) {
        if (other.sides[os].neighbour === si) other.sides[os] = wall(texture);
    }
    level.segments[si].sides[side] = wall(texture);
    rebuildDerived(level);
}

export { MIRROR, CUBE_CORNERS, SIDE_AXIS };
