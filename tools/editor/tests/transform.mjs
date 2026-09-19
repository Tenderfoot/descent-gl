// Transforming more than one thing at once.
//
// The maths is small and the interesting cases are all about what a GROUP does
// that a single selection does not:
//
//   * a rotate or a scale is about the selection's centre, so it has to be
//     applied to the positions recorded when the drag started. The gizmo
//     reports absolute state, and applying that to its own last output
//     compounds - sixty times a second, for the length of a drag.
//   * two segments sharing a vertex deform together, which is the shared vertex
//     array doing its job and the reason the editor has no vertex-soup mode.
//   * nothing in either validator catches an inverted segment, so a negative
//     scale must be unreachable rather than merely discouraged.
//   * an entity carries a cached `segment` the loader TRUSTS, so every member of
//     a moved group has to have it re-resolved, not just the one clicked.
//
//   node tools/editor/tests/transform.mjs

import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { SIDE_VERTICES } from '../src/format.js';
import { faceNormal, findSegment, sideCorners } from '../src/geometry.js';
import { emitJson } from '../src/json_emit.js';
import { parseLevel, serializeLevel } from '../src/level.js';
import { moveVertices } from '../src/ops.js';
import {
    addLight, addObject, entityFacing, moveEntities, turnEntity,
} from '../src/entities.js';
import {
    centroid, fromLevelQuat, groupTransform, MIN_SCALE, sanitiseScale, toLevelQuat,
    transformPoint, transformPoints, isIdentityTransform,
} from '../src/transform.js';
import { validateLevel } from '../src/validate.js';

const GAME = process.env.DESCENT_BIN ?? 'build/Debug/descent';
const BASE = 'tools/editor/testdata/base.json';

let failures = 0;
const report = (ok, label, detail = '') => {
    if (!ok) ++failures;
    console.log(`${ok ? 'ok  ' : 'FAIL'}  ${label}${detail ? `\n        ${detail}` : ''}`);
};

const fresh = () => parseLevel(fs.readFileSync(BASE, 'utf8'), BASE);
const close = (a, b, tol = 1e-6) => Math.abs(a - b) <= tol;
const closeVec = (a, b, tol = 1e-6) => a.every((n, k) => close(n, b[k], tol));

// A rotation of `degrees` about `axis`, as the gizmo would report it.
function quatAxisAngle(axis, degrees) {
    const half = (degrees * Math.PI) / 360;
    const s = Math.sin(half);
    const length = Math.hypot(...axis) || 1;
    return [axis[0] / length * s, axis[1] / length * s, axis[2] / length * s, Math.cos(half)];
}

// --- the transform itself ---------------------------------------------------

{
    const points = [[0, 0, 0], [4, 0, 0], [4, 0, 4], [0, 0, 4]];
    const pivot = centroid(points);
    report(closeVec(pivot, [2, 0, 2]), 'the pivot is the middle of the selection',
           `got ${pivot}`);

    const t = groupTransform({ pivot, translate: [10, -3, 0] });
    const moved = transformPoints(t, points);
    const shifted = moved.every((p, i) => closeVec(p, [points[i][0] + 10, points[i][1] - 3, points[i][2]]));
    report(shifted, 'a translate moves every point by the same delta');

    // The shape is what a group transform must not change.
    const spread = (list) => {
        const middle = centroid(list);
        return list.map((p) => Math.hypot(p[0] - middle[0], p[1] - middle[1], p[2] - middle[2]));
    };
    report(closeVec(spread(points), spread(moved)),
           'and leaves every point the same distance from the middle');
}

{
    // A quarter turn about Y maps +X to -Z, about the selection's own centre.
    const points = [[0, 0, 0], [4, 0, 0]];
    const pivot = centroid(points);
    const t = groupTransform({ pivot, rotation: quatAxisAngle([0, 1, 0], 90) });
    const [a, b] = transformPoints(t, points);
    report(closeVec(a, [2, 0, 2]) && closeVec(b, [2, 0, -2]),
           'a rotate turns the selection about its own centre', `got ${a} and ${b}`);
}

{
    const points = [[0, 0, 0], [4, 0, 0]];
    const t = groupTransform({ pivot: centroid(points), scale: [2, 2, 2] });
    const [a, b] = transformPoints(t, points);
    report(closeVec(a, [-2, 0, 0]) && closeVec(b, [6, 0, 0]),
           'a scale spreads the selection about its own centre', `got ${a} and ${b}`);
}

// Applying the same absolute transform twice must land in the same place. This
// is the property that makes it safe to run once per frame of a drag against
// the positions recorded at the start of it.
{
    const points = [[1, 2, 3], [5, -2, 0], [0, 0, 7]];
    const t = groupTransform({
        pivot: centroid(points),
        translate: [3, 3, 3],
        rotation: quatAxisAngle([1, 1, 0], 37),
        scale: [1.5, 1.5, 1.5],
    });
    const once = transformPoints(t, points);
    const again = transformPoints(t, points);
    report(once.every((p, i) => closeVec(p, again[i])),
           'the transform is a pure function of the positions it started from');

    // And the thing that would go wrong if it were not: feeding the output back
    // in drifts, which after a few hundred frames of a drag is a deformed group.
    const fed = transformPoints(t, once);
    report(!once.every((p, i) => closeVec(p, fed[i])),
           'while feeding its own output back in does not, which is why it does not');
}

{
    const identity = groupTransform({ pivot: [1, 2, 3] });
    report(isIdentityTransform(identity), 'an undragged gizmo describes the identity');
    report(!isIdentityTransform(groupTransform({ pivot: [1, 2, 3], translate: [0, 0.001, 0] })),
           'and a nudged one does not');
}

// --- the negative-scale guard ----------------------------------------------

{
    // Neither validator has a winding rule, so a mirrored segment references the
    // same vertices in the same order, passes every structural check, and
    // renders with its normals pointing out of the mine. The handle must not be
    // able to express it at all.
    const clamped = sanitiseScale([-2, 0, 1]);
    report(clamped.every((n) => n >= MIN_SCALE) && close(clamped[0], 2),
           'a negative or zero scale is clamped to a positive one', `got ${clamped}`);

    const level = fresh();
    const seg = level.segments[1];
    const points = seg.verts.map((v) => level.vertices[v]);
    const before = faceNormal(...sideCorners(level, 1, 0).slice(0, 3));

    const t = groupTransform({ pivot: centroid(points), scale: [-1, 1, 1] });
    moveVertices(level, seg.verts.map((v, i) => [v, transformPoint(t, points[i])]));
    const after = faceNormal(...sideCorners(level, 1, 0).slice(0, 3));

    const dot = before[0] * after[0] + before[1] * after[1] + before[2] * after[2];
    report(dot > 0, 'so a mirroring drag cannot turn a segment inside out',
           `face normal flipped: dot=${dot.toFixed(3)}`);
}

// --- moveVertices against the shared vertex array --------------------------

{
    const level = fresh();
    // The portal between segments 0 and 1 shares its four vertices by index.
    let portalSide = -1;
    for (let side = 0; side < 6; ++side) {
        if (level.segments[0].sides[side].neighbour === 1) { portalSide = side; break; }
    }
    report(portalSide >= 0, 'the base level has a portal to deform through');

    const shared = SIDE_VERTICES[portalSide].map((c) => level.segments[0].verts[c]);
    const points = shared.map((v) => level.vertices[v].slice());
    const t = groupTransform({ pivot: centroid(points), scale: [1.2, 1.2, 1.2] });

    moveVertices(level, shared.map((v, i) => [v, transformPoint(t, points[i])]));

    const ownSide = sideCorners(level, 0, portalSide);
    const theirSide = sideCorners(level, 1, level.segments[0].sides[portalSide].neighbour >= 0
        ? level.segments[1].sides.findIndex((sd) => sd.neighbour === 0)
        : 0);
    const agree = ownSide.every((c) => theirSide.some((d) => closeVec(c, d, 1e-9)));
    report(agree, 'widening a portal moves both segments’ copies of it, because there is one copy');

    const { findings } = validateLevel(level);
    const errors = findings.filter((f) => f.severity === 'error');
    report(errors.length === 0, 'and the level still validates',
           errors.map((f) => f.message).join('; '));
}

{
    const level = fresh();
    const before = level.vertices.map((v) => v.slice());
    let refused = false;
    try {
        moveVertices(level, [[0, [1, 1, 1]], [999, [2, 2, 2]]]);
    } catch { refused = true; }
    report(refused, 'moveVertices refuses an index that does not exist');
    report(level.vertices.every((v, i) => closeVec(v, before[i])),
           'and writes nothing at all when it does, so the undo entry stays honest');
}

// One rebuild for the group, not one per vertex: the derived data has to match
// the FINAL positions, which is the thing a per-vertex loop gets right by
// accident and a partial rebuild gets wrong.
{
    const level = fresh();
    const seg = level.segments[1];
    const points = seg.verts.map((v) => level.vertices[v].slice());
    const t = groupTransform({ pivot: centroid(points), translate: [7, 0, 0] });
    moveVertices(level, seg.verts.map((v, i) => [v, transformPoint(t, points[i])]));

    const want = centroid(seg.verts.map((v) => level.vertices[v]));
    report(closeVec(level.segments[1].center, want),
           'the derived centres and planes match the positions the group ended at',
           `centre ${level.segments[1].center} want ${want}`);
}

// --- entities ---------------------------------------------------------------

{
    const level = fresh();
    const centre0 = level.segments[0].center.slice();
    const centre1 = level.segments[1].center.slice();
    const a = addLight(level, centre0.slice());
    const b = addLight(level, centre0.slice());
    report(level.lights[a].segment === 0 && level.lights[b].segment === 0,
           'two lights placed in segment 0 are recorded as being in it');

    // Move them both into segment 1. Both cached segments have to follow, not
    // just the one that happened to be clicked: the loader trusts a `segment`
    // that is merely wrong, so a stale one is a light that lights another room.
    const delta = [centre1[0] - centre0[0], centre1[1] - centre0[1], centre1[2] - centre0[2]];
    const t = groupTransform({ pivot: centre0, translate: delta });
    const moved = transformPoints(t, [level.lights[a].position, level.lights[b].position]);
    moveEntities(level, [
        { kind: 'light', index: a, position: moved[0] },
        { kind: 'light', index: b, position: moved[1] },
    ]);
    report(level.lights[a].segment === 1 && level.lights[b].segment === 1,
           'and both have their cached segment re-resolved when the group moves',
           `got ${level.lights[a].segment} and ${level.lights[b].segment}`);
    report(findSegment(level, level.lights[a].position) === level.lights[a].segment,
           'which is the segment that actually contains them');
}

{
    const level = fresh();
    const index = addObject(level, 'pillar', level.segments[1].center.slice());
    const base = entityFacing(level, 'object', index);
    const quarter = quatAxisAngle([0, 1, 0], 90);

    // The gizmo reports the whole turn so far, every frame. Composing it onto
    // the object's current facing once per frame would apply it sixty times a
    // second; given the facing it started from, it is idempotent.
    turnEntity(level, 'object', index, quarter, base);
    const after = level.objects[index].orientation.slice();
    turnEntity(level, 'object', index, quarter, base);
    report(closeVec(level.objects[index].orientation, after),
           'turning an object from a recorded base is idempotent, so a drag cannot compound it');

    turnEntity(level, 'object', index, quarter, null);
    const compounded = level.objects[index].orientation;
    report(!closeVec(compounded, after),
           'while turning it from its current facing does compound, which is why the base exists');

    // A quarter turn about Y, twice, is a half turn: the composition is real and
    // not just a value being written.
    const half = quatAxisAngle([0, 1, 0], 180);
    report(closeVec(fromLevelQuat(compounded).map(Math.abs), half.map(Math.abs), 1e-6),
           'and two quarter turns make a half one');
}

{
    // A point light has a `direction` field that nothing reads. Writing one
    // would put a cone in the file for a light that has none.
    const level = fresh();
    const index = addLight(level, level.segments[1].center.slice());
    const before = level.lights[index].direction.slice();
    turnEntity(level, 'light', index, quatAxisAngle([1, 0, 0], 90));
    report(closeVec(level.lights[index].direction, before),
           'a point light’s unread direction is left alone by a group rotate');

    level.lights[index].type = 'spot';
    level.lights[index].direction = [0, -1, 0];
    turnEntity(level, 'light', index, quatAxisAngle([1, 0, 0], 90), [0, -1, 0]);
    report(closeVec(level.lights[index].direction, [0, 0, -1]),
           'while a spot’s aim turns with the group', `got ${level.lights[index].direction}`);
}

{
    const level = fresh();
    report(entityFacing(level, 'pickup', 0) === null || level.pickups.length === 0,
           'a pickup has no facing for a rotate to turn');
    report(toLevelQuat(fromLevelQuat([1, 0, 0, 0])).join() === '1,0,0,0',
           'the file’s [w,x,y,z] and the gizmo’s [x,y,z,w] round-trip');
}

// --- the whole thing, through the writer and the game ----------------------

{
    const level = fresh();
    const seg = level.segments[1];
    const points = seg.verts.map((v) => level.vertices[v].slice());
    const t = groupTransform({
        pivot: centroid(points),
        translate: [1.5, 0, 0],
        scale: [1.1, 1.1, 1.1],
    });
    moveVertices(level, seg.verts.map((v, i) => [v, transformPoint(t, points[i])]));

    const { findings, ok } = validateLevel(level);
    const errors = findings.filter((f) => f.severity === 'error');
    report(ok && errors.length === 0, 'a transformed level validates',
           errors.map((f) => f.message).join('; '));

    const text = emitJson(serializeLevel(level, 'base'));
    const reread = parseLevel(text, 'transformed.json');
    const same = reread.vertices.every((v, i) => closeVec(v, level.vertices[i], 1e-6));
    report(same, 'and round-trips through the writer with its new vertices');

    if (fs.existsSync(GAME)) {
        const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'descent-transform-'));
        const file = path.join(tmp, 'transformed.json');
        fs.writeFileSync(file, text);
        let loaded = false;
        let detail = '';
        try {
            const out = execFileSync(GAME, ['--world-test', '--level', file],
                                     { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
            loaded = /level '.*transformed\.json'/.test(out);
        } catch (e) {
            const output = `${e.stdout ?? ''}${e.stderr ?? ''}`;
            // --world-test also judges whether the level is any GOOD, which a
            // three-segment test level is not. Only an abort counts against us.
            loaded = !/\[FATAL\]/.test(output);
            detail = (output.split('\n').filter((l) => l.includes('[ERROR]')).pop() ?? '').trim();
        }
        report(loaded, 'and the game loads it without aborting', loaded ? '' : detail);
        fs.rmSync(tmp, { recursive: true, force: true });
    } else {
        console.log(`\n  note: ${GAME} not found, so the game half is skipped.\n`);
    }
}

console.log(`\n${failures === 0 ? 'transform: PASS' : `transform: FAIL (${failures})`}`);
process.exit(failures === 0 ? 0 : 1);
