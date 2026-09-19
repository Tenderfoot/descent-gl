// The mine generator, checked for the promises it makes.
//
// Three of them are worth a test each, because each is a claim the author acts
// on. A seed reproduces a mine, or there is no point showing one. The number
// of ways round is what was asked for, or the setting is decoration. And every
// key can be reached before the door it opens, which is the one that decides
// whether a generated mine can be finished at all - and the one that cannot be
// seen by looking at the level.
//
// The fourth is the surface: the union of every room's walls has to be one
// closed sphere. It is checked here rather than left to the game because the
// shape that breaks it is easy to produce by accident and invisible in the
// editor - a ring of rooms round an empty cell is a doughnut, and its wall
// surface is a torus.
//
//   node tools/editor/tests/generate.mjs

import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { insideSegment } from '../src/geometry.js';
import { emitJson } from '../src/json_emit.js';
import { serializeLevel } from '../src/level.js';
import { generateMine } from '../src/generate.js';
import { validateLevel } from '../src/validate.js';

const GAME = process.env.DESCENT_BIN ?? 'build/Debug/descent';

let failures = 0;
const report = (ok, label, detail = '') => {
    if (!ok) ++failures;
    console.log(`${ok ? 'ok  ' : 'FAIL'}  ${label}${detail ? `\n        ${detail}` : ''}`);
};

// Independent cycles in the segment graph: edges - rooms + 1. This is the
// number the generator calls "ways round", counted from the level it produced
// rather than from the lattice it was planned on.
function waysRound(level) {
    let portals = 0;
    for (const seg of level.segments) {
        for (const sd of seg.sides) if (sd.neighbour >= 0) ++portals;
    }
    return portals / 2 - level.segments.length + 1;
}

// Which rooms can be reached from the start without opening `lockedOut`.
function reachable(level, lockedOut) {
    const blocked = new Set();
    for (const door of level.doors) {
        if (!lockedOut.has(door.key)) continue;
        blocked.add(`${door.segment}:${door.side}`);
        const sd = level.segments[door.segment].sides[door.side];
        blocked.add(`${sd.neighbour}:${level.segments[sd.neighbour].sides
            .findIndex((s) => s.neighbour === door.segment)}`);
    }

    const seen = new Set([level.start.segment]);
    const queue = [level.start.segment];
    while (queue.length > 0) {
        const si = queue.shift();
        for (let side = 0; side < 6; ++side) {
            const sd = level.segments[si].sides[side];
            if (sd.neighbour < 0 || blocked.has(`${si}:${side}`)) continue;
            if (seen.has(sd.neighbour)) continue;
            seen.add(sd.neighbour);
            queue.push(sd.neighbour);
        }
    }
    return seen;
}

const SIZES = [
    { rooms: 10, loops: 1, keys: 0 },
    { rooms: 18, loops: 3, keys: 1 },
    { rooms: 26, loops: 5, keys: 2 },
    { rooms: 40, loops: 9, keys: 3 },
];

console.log('generate\n');

// --- a seed reproduces a mine ---------------------------------------------
{
    const options = { rooms: 24, loops: 4, keys: 2, seed: 20260919 };
    const first = generateMine('Repeat', options);
    const again = generateMine('Repeat', { ...options, seed: first.report.seed });

    report(again.report.attempts === 1,
           'the seed a roll reports needs no re-rolling to reproduce',
           `took ${again.report.attempts}`);
    report(JSON.stringify(first.level.vertices) === JSON.stringify(again.level.vertices) &&
           JSON.stringify(first.level.segments) === JSON.stringify(again.level.segments),
           'and gives back the same rooms in the same places');
    report(JSON.stringify([first.level.objects, first.level.pickups, first.level.lights,
                           first.level.doors]) ===
           JSON.stringify([again.level.objects, again.level.pickups, again.level.lights,
                           again.level.doors]),
           'and the same things in them');

    const different = generateMine('Repeat', { ...options, seed: first.report.seed + 1 });
    report(JSON.stringify(different.level.segments) !== JSON.stringify(first.level.segments),
           'while the next seed along is a different mine');
}

// --- the settings mean what they say ---------------------------------------
console.log('');
for (const size of SIZES) {
    const label = `${size.rooms} rooms, ${size.loops} ways round, ${size.keys} key(s)`;
    let roomsOk = true;
    let loopsOk = true;
    let validates = true;
    let sphere = true;
    let contained = true;
    let detail = '';

    for (const seed of [3, 17, 404]) {
        const { level } = generateMine('Sized', { ...size, robots: 6, seed });

        if (level.segments.length !== size.rooms) {
            roomsOk = false;
            detail ||= `seed ${seed} made ${level.segments.length} rooms`;
        }
        if (waysRound(level) !== size.loops) {
            loopsOk = false;
            detail ||= `seed ${seed} has ${waysRound(level)} ways round`;
        }

        const result = validateLevel(level);
        if (!result.ok) {
            validates = false;
            detail ||= `seed ${seed}: ${result.findings.find((f) => f.severity === 'error').message}`;
        }
        const c = result.closure;
        const euler = c ? c.weldedVertices - c.edges + c.triangles : null;
        if (euler !== 2) {
            sphere = false;
            detail ||= `seed ${seed}: Euler characteristic ${euler}`;
        }
        for (let si = 0; si < level.segments.length; ++si) {
            if (!insideSegment(level, si, level.segments[si].center, 0.0)) {
                contained = false;
                detail ||= `seed ${seed}: room ${si} does not contain its own centre`;
            }
        }
    }

    report(roomsOk, `${label}: makes exactly the rooms asked for`, roomsOk ? '' : detail);
    report(loopsOk, `${label}: makes exactly the ways round asked for`, loopsOk ? '' : detail);
    report(validates, `${label}: passes every rule the loader enforces`, validates ? '' : detail);
    report(sphere, `${label}: one closed wall surface, genus 0`, sphere ? '' : detail);
    report(contained, `${label}: every room contains its own centre`, contained ? '' : detail);
}

// --- the key chain can actually be walked ----------------------------------
//
// The claim is that key j sits somewhere reachable with keys 0..j-1 in hand.
// Checked by locking every door from key j onwards and confirming the pickup
// for key j is in a room the player can still get to.
console.log('');
{
    let chainOk = true;
    let orderOk = true;
    let detail = '';

    for (const seed of [11, 29, 77, 512]) {
        const { level } = generateMine('Chain', { rooms: 34, loops: 6, keys: 3, robots: 8, seed });
        const keyed = level.doors.filter((d) => d.key >= 0).map((d) => d.key).sort();

        for (const k of keyed) {
            const stillLocked = new Set(keyed.filter((other) => other >= k));
            const open = reachable(level, stillLocked);
            const pickup = level.pickups.find((p) => p.kind === 6 + k);
            if (!pickup) {
                chainOk = false;
                detail ||= `seed ${seed}: door needs key ${k} but no such key is placed`;
                continue;
            }
            if (!open.has(pickup.segment)) {
                chainOk = false;
                detail ||= `seed ${seed}: key ${k} is behind the door it opens`;
            }
        }

        // And the exit has to be reachable once every key is in hand.
        if (level.exitTrigger) {
            const all = reachable(level, new Set());
            if (!all.has(level.exitTrigger.segment)) {
                orderOk = false;
                detail ||= `seed ${seed}: the exit cannot be reached at all`;
            }
        } else {
            orderOk = false;
            detail ||= `seed ${seed}: no exit`;
        }
    }

    report(chainOk, 'every key is reachable before the door it opens', chainOk ? '' : detail);
    report(orderOk, 'and the way out is reachable with all of them', orderOk ? '' : detail);
}

// --- and the game will load it ---------------------------------------------
console.log('');
if (fs.existsSync(GAME)) {
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'descent-generate-'));
    const file = path.join(tmp, 'Generated.json');
    const { level } = generateMine('Generated', { rooms: 26, loops: 5, keys: 2, robots: 8, seed: 8 });
    fs.writeFileSync(file, emitJson(serializeLevel(level)));

    // --world-test is the one that judges the SHAPE: closed surface, Euler
    // characteristic, portal culling doing something, somewhere dark to stand.
    // A generated mine is meant to satisfy all of it, unlike the six-segment
    // fixture the other tests use, so this is asserted rather than excused.
    let ok = true;
    let detail = '';
    try {
        execFileSync(GAME, ['--world-test', '--level', file],
                     { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
    } catch (e) {
        ok = false;
        const text = `${e.stdout ?? ''}${e.stderr ?? ''}`;
        detail = (text.split('\n').filter((l) => l.includes('[ERROR]'))[0] ?? '').trim();
    }
    report(ok, 'the game\'s own --world-test passes on a generated mine', ok ? '' : detail);

    let rays = true;
    let rayDetail = '';
    try {
        execFileSync(GAME, ['--collision-test', '--level', file],
                     { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
    } catch (e) {
        rays = false;
        const text = `${e.stdout ?? ''}${e.stderr ?? ''}`;
        rayDetail = (text.split('\n').filter((l) => l.includes('[ERROR]'))[0] ?? '').trim();
    }
    report(rays, 'and --collision-test: no ray escapes it', rays ? '' : rayDetail);

    fs.rmSync(tmp, { recursive: true, force: true });
} else {
    console.log(`  note: ${GAME} not found, so the game half is skipped.\n`);
}

console.log(`\n${failures === 0 ? 'generate: PASS' : `generate: FAIL (${failures})`}`);
process.exit(failures === 0 ? 0 : 1);
