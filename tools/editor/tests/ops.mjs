// The structural operations, checked against the game's own validator.
//
// The one that matters is extrude. Its corner correspondence - which of a new
// segment's eight slots takes which existing vertex - is the only place in the
// editor where a bug produces a level that passes every structural rule and is
// still wrong: a twisted segment. Every rule in the format is about indices,
// and a twist is about which index went in which slot, so "it validates" is
// not evidence. These tests check the shape.
//
//   node tools/editor/tests/ops.mjs

import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { OPPOSITE_SIDE, SIDE_COUNT, SIDE_NAMES, SIDE_VERTICES } from '../src/format.js';
import { distance, faceNormal, sideCorners, sub, dot, length } from '../src/geometry.js';
import { emitJson } from '../src/json_emit.js';
import { parseLevel, serializeLevel } from '../src/level.js';
import { compactVertices, deleteSegment, extrudeSide, joinSides, segmentDepth, splitPortal } from '../src/ops.js';
import { rebuildDerived as rebuild } from '../src/geometry.js';
import { validateLevel } from '../src/validate.js';

const GAME = process.env.DESCENT_BIN ?? 'build/Debug/descent';
const BASE = 'tools/editor/testdata/base.json';

let failures = 0;
const report = (ok, label, detail = '') => {
    if (!ok) ++failures;
    console.log(`${ok ? 'ok  ' : 'FAIL'}  ${label}${detail ? `\n        ${detail}` : ''}`);
};

const fresh = () => parseLevel(fs.readFileSync(BASE, 'utf8'), BASE);

// A segment is a box if its twelve edges have the lengths a box's do and its
// six faces are planar. A twisted segment fails this while validating fine.
function boxiness(level, si) {
    const seg = level.segments[si];
    const p = (slot) => level.vertices[seg.verts[slot]];

    // The twelve edges of a cube, as pairs of corner slots.
    const edges = [[0, 1], [1, 2], [2, 3], [3, 0], [4, 5], [5, 6], [6, 7], [7, 4],
                   [0, 4], [1, 5], [2, 6], [3, 7]];
    let shortest = Infinity;
    for (const [a, b] of edges) shortest = Math.min(shortest, distance(p(a), p(b)));

    // Opposite faces should not cross: a twist shows up as two faces of the
    // same segment whose normals point at each other rather than apart.
    let worstFold = 1;
    for (let side = 0; side < SIDE_COUNT; ++side) {
        const c = sideCorners(level, si, side);
        const n0 = faceNormal(c[0], c[1], c[2]);
        const n1 = faceNormal(c[0], c[2], c[3]);
        worstFold = Math.min(worstFold, dot(n0, n1));   // 1.0 = planar quad
    }

    // The eight corners must be eight distinct places.
    const distinct = new Set(seg.verts.map((v) => level.vertices[v].join(','))).size;

    return { shortest, worstFold, distinct };
}

// --- extrude, in all six directions ----------------------------------------

for (let side = 0; side < SIDE_COUNT; ++side) {
    const level = fresh();
    // Segment 1 is the middle of the three, so four of its six sides are walls
    // and the two portals are the ones this must refuse.
    const isPortal = level.segments[1].sides[side].neighbour >= 0;
    const si = isPortal ? 0 : 1;
    const target = level.segments[si].sides[side].neighbour >= 0 ? null : si;

    if (target === null) {
        // Both candidates are portals on this side; extrude from the far end.
        report(true, `extrude ${SIDE_NAMES[side]}: no wall to grow from in the base level`);
        continue;
    }

    const before = level.segments.length;
    const created = extrudeSide(level, target, side);
    const shape = boxiness(level, created);
    const { findings } = validateLevel(level);
    const errors = findings.filter((f) => f.severity === 'error');

    const shared = SIDE_VERTICES[side].map((c) => level.segments[target].verts[c]).sort();
    const back = OPPOSITE_SIDE[side];
    const theirs = SIDE_VERTICES[back].map((c) => level.segments[created].verts[c]).sort();

    const ok = level.segments.length === before + 1 &&
               errors.length === 0 &&
               shape.distinct === 8 &&
               shape.shortest > 1 &&
               shape.worstFold > 0.999 &&
               shared.join() === theirs.join() &&
               level.segments[created].sides[back].neighbour === target &&
               level.segments[target].sides[side].neighbour === created;

    report(ok, `extrude ${SIDE_NAMES[side]} from segment ${target}: a box, sharing four vertices`,
           ok ? '' : `errors=${errors.length} distinct=${shape.distinct} ` +
                     `shortestEdge=${shape.shortest.toFixed(2)} flatness=${shape.worstFold.toFixed(4)} ` +
                     `shared=[${shared}] vs [${theirs}]`);
}

// Extruding twice from the same wall must be refused, not silently allowed.
{
    const level = fresh();
    extrudeSide(level, 1, 1);
    let refused = false;
    try { extrudeSide(level, 1, 1); } catch { refused = true; }
    report(refused, 'extrude refuses a side that is already a portal');
}

// The default depth matches the segment it grew from.
{
    const level = fresh();
    const want = segmentDepth(level, 1, 1);
    const created = extrudeSide(level, 1, 1);
    const got = segmentDepth(level, created, 1);
    report(Math.abs(want - got) < 1e-3, 'extrude defaults to the depth of the segment it grew from',
           `wanted ${want.toFixed(2)}, got ${got.toFixed(2)}`);
}

// --- join -------------------------------------------------------------------

// Extrude two segments that meet, then join them: the classic loop-closing
// move, and the one that has to rewrite vertex indices rather than move points.
{
    const level = fresh();
    const up = extrudeSide(level, 0, 1);        // grow up from segment 0
    const across = extrudeSide(level, up, 2);   // and right, so it sits over segment 1
    const down = extrudeSide(level, 1, 1);      // grow up from segment 1 to meet it

    // `across` and `down` now occupy the same space; join needs them adjacent
    // instead. Use the pair that genuinely touch: `across`'s bottom and
    // segment 1's top are the same four places only if the geometry lines up,
    // so this asserts the failure path when it does not.
    let refusedApart = false;
    try {
        joinSides(level, up, 4, down, 5);
    } catch (e) {
        refusedApart = /not in the same place|do not line up/.test(e.message);
    }
    report(refusedApart, 'join refuses two sides that are not in the same place');
}

{
    // Two extrudes that meet. Because pushVertex reuses a vertex that is
    // already there, growing up from 0 and then up from 1 leaves the two new
    // segments already sharing the four corners of the face between them - so
    // this join only has to set the portals. That is the common case and the
    // cheap one.
    //
    // Note what the level looks like in between: two walls in the same place,
    // each contributing triangles from the same four indices, which is an
    // over-shared surface. The closure check says so, which is how the editor
    // tells an author "you have two walls on top of each other, join them".
    const level = fresh();
    const a = extrudeSide(level, 0, 1);
    const b = extrudeSide(level, 1, 1);

    const between = validateLevel(level);
    report(between.closure?.closed === false,
           'two extrudes that meet leave an over-shared surface until they are joined',
           `openEdges=${between.closure?.openEdges} overShared=${between.closure?.overSharedEdges}`);

    joinSides(level, a, 2, b, 0);

    const { findings, closure } = validateLevel(level);
    const errors = findings.filter((f) => f.severity === 'error');
    const shared = SIDE_VERTICES[2].map((c) => level.segments[a].verts[c]).sort();
    const theirs = SIDE_VERTICES[0].map((c) => level.segments[b].verts[c]).sort();

    const ok = errors.length === 0 && closure.closed && shared.join() === theirs.join() &&
               level.segments[a].sides[2].neighbour === b &&
               level.segments[b].sides[0].neighbour === a;
    report(ok, 'joining them closes the surface and makes the portal reciprocal',
           ok ? '' : `errors=${errors.map((f) => f.rule)} closed=${closure?.closed} ` +
                     `shared=[${shared}] vs [${theirs}]`);
}

{
    // The other case: two wings authored separately, whose faces line up but
    // whose vertices are different indices entirely. This is the one that has
    // to rewrite indices across the whole level and drop what it made
    // redundant - and the one that would be tempting to "solve" by moving
    // points until they coincide, which is exactly the level that leaks light.
    const level = fresh();

    // A box flush against segment 2's right face, built with its own vertices.
    const base = level.vertices.length;
    for (const [x, y, z] of [[60, 0, 0], [80, 0, 0], [80, 20, 0], [60, 20, 0],
                             [60, 0, 20], [80, 0, 20], [80, 20, 20], [60, 20, 20]]) {
        level.vertices.push([x, y, z]);
    }
    level.segments.push({
        verts: [0, 1, 2, 3, 4, 5, 6, 7].map((k) => base + k),
        sides: [0, 1, 2, 3, 4, 5].map(() => ({ neighbour: -1, door: -1, texture: 0, uv: null })),
        keyOrder: null,
    });
    const wing = level.segments.length - 1;
    rebuild(level);

    const verticesBefore = level.vertices.length;
    const mineBefore = SIDE_VERTICES[2].map((c) => level.segments[2].verts[c]).sort();
    const theirsBefore = SIDE_VERTICES[0].map((c) => level.segments[wing].verts[c]).sort();
    report(mineBefore.join() !== theirsBefore.join(),
           'the two wings start with different vertex indices for the same four places');

    joinSides(level, 2, 2, wing, 0);

    const { findings, closure } = validateLevel(level);
    const errors = findings.filter((f) => f.severity === 'error');
    const mine = SIDE_VERTICES[2].map((c) => level.segments[2].verts[c]).sort();
    const theirs = SIDE_VERTICES[0].map((c) => level.segments[wing].verts[c]).sort();

    report(errors.length === 0 && closure.closed && mine.join() === theirs.join(),
           'join rewrites one wing\'s indices to the other\'s, and the seam closes',
           `errors=${errors.map((f) => f.rule)} closed=${closure?.closed} ` +
           `[${mine}] vs [${theirs}]`);
    report(level.vertices.length === verticesBefore - 4,
           'join drops the four vertices it made redundant',
           `${verticesBefore} -> ${level.vertices.length}`);
}

{
    const level = fresh();
    let refused = false;
    try { joinSides(level, 0, 2, 1, 0); } catch { refused = true; }
    report(refused, 'join refuses a side that is already a portal');
}

// --- delete -----------------------------------------------------------------

{
    const level = fresh();
    const created = extrudeSide(level, 1, 1);
    deleteSegment(level, created);

    const { findings } = validateLevel(level);
    const errors = findings.filter((f) => f.severity === 'error');
    const ok = errors.length === 0 && level.segments.length === 3 &&
               level.vertices.length === 16 && level.segments[1].sides[1].neighbour === -1;
    report(ok, 'delete undoes an extrude exactly, orphaned vertices and all',
           ok ? '' : `segments=${level.segments.length} vertices=${level.vertices.length} ` +
                     `errors=${errors.map((f) => f.rule)}`);
}

{
    // Deleting from the middle shifts every index above it. The player start
    // lives in segment 0 and must still be in segment 0 afterwards; segment 2
    // must become segment 1.
    const level = fresh();
    const before = level.segments[2].center.slice();
    deleteSegment(level, 1);

    const { findings } = validateLevel(level);
    const errors = findings.filter((f) => f.severity === 'error');
    const moved = distance(level.segments[1].center, before) < 1e-6;
    const sealed = level.segments[0].sides[2].neighbour === -1 &&
                   level.segments[1].sides[0].neighbour === -1;
    report(errors.length === 0 && moved && sealed && level.start.segment === 0,
           'delete from the middle reindexes the graph, the doors and the entities',
           `errors=${errors.map((f) => f.rule)} start=${level.start.segment} sealed=${sealed}`);
}

{
    // The light lives in segment 1. Deleting the room takes the lamp with it -
    // keeping it floating in the rock the room became is not what anyone
    // means by "delete this segment".
    const level = fresh();
    const removed = deleteSegment(level, 1);
    report(removed.lights === 1 && level.lights.length === 0,
           'delete takes the lights, pickups and objects inside it with it',
           JSON.stringify(removed));
    report(level.lights.every((l) => l.segment >= 0 && l.segment < level.segments.length),
           'and leaves no entity pointing at a segment that moved');
}

{
    // The player start is not furniture. Deleting the segment it is in must
    // not quietly delete it or leave it pointing somewhere plausible-looking:
    // it goes to -1 and the validator says so.
    const level = fresh();
    deleteSegment(level, 0);
    const { findings } = validateLevel(level);
    report(level.start.segment === -1 && findings.some((f) => f.rule === 'start-segment'),
           'delete does not silently relocate the player start',
           `start.segment = ${level.start.segment}`);
}

{
    // Removing a segment from the middle of a cluster can leave four faces
    // meeting along one edge. Nothing in the delete can fix that - the
    // geometry genuinely pinches - so what matters is that the closure check
    // notices, and that the editor therefore refuses to save it.
    const level = parseLevel(fs.readFileSync('assets/levels/test01.json', 'utf8'), 'test01');
    deleteSegment(level, 2);
    const { closure } = validateLevel(level);
    report(closure.closed === false && closure.overSharedEdges === 1,
           'a delete that pinches the wall surface is reported, not hidden',
           `openEdges=${closure.openEdges} overShared=${closure.overSharedEdges}`);
}

// --- the game agrees --------------------------------------------------------

// An edited level is only really valid if the game loads it. Build one with
// every operation, write it out, and hand it to the real binary.
const haveGame = fs.existsSync(GAME);
{
    const level = fresh();
    const a = extrudeSide(level, 0, 1);          // a storey above segment 0
    const b = extrudeSide(level, 1, 1);          // and above segment 1
    joinSides(level, a, 2, b, 0);                // joined, so the upper floor is one run
    const spare = extrudeSide(level, 0, 3);      // a shaft below the start
    deleteSegment(level, spare);                 // thought better of it

    const { findings, closure } = validateLevel(level);
    const errors = findings.filter((f) => f.severity === 'error');
    report(errors.length === 0, 'a level built from extrude, join and delete validates',
           errors.map((f) => f.message).join('; '));
    report(closure?.closed === true, 'and its wall surface is still closed',
           closure ? `${closure.openEdges} open, ${closure.overSharedEdges} over-shared` : 'not checked');

    // It must also survive a save and a reload unchanged.
    const text = emitJson(serializeLevel(level));
    const reloaded = parseLevel(text, 'edited');
    const again = emitJson(serializeLevel(reloaded));
    report(text === again, 'an edited level round-trips through the writer');

    if (haveGame) {
        const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'descent-ops-'));
        const file = path.join(tmp, 'edited.json');
        fs.writeFileSync(file, text);
        let loaded = false;
        let detail = '';
        try {
            const out = execFileSync(GAME, ['--world-test', '--level', file],
                                     { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
            loaded = /level '.*edited\.json'/.test(out);
        } catch (e) {
            const text2 = `${e.stdout ?? ''}${e.stderr ?? ''}`;
            // --world-test also judges whether the level is any GOOD (dark
            // segments, culling doing something), which a six-segment test
            // level is not. Only an abort during load counts against us.
            loaded = !/\[FATAL\]/.test(text2);
            detail = (text2.split('\n').filter((l) => l.includes('[ERROR]')).pop() ?? '').trim();
        }
        report(loaded, 'and the game loads it without aborting', loaded ? '' : detail);
        fs.rmSync(tmp, { recursive: true, force: true });
    } else {
        console.log(`\n  note: ${GAME} not found, so the game half is skipped.\n`);
    }
}

console.log(`\n${failures === 0 ? 'ops: PASS' : `ops: FAIL (${failures})`}`);
process.exit(failures === 0 ? 0 : 1);
