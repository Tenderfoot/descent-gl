// Does the editor's validator agree with the game's?
//
// Two validators for one format will drift, and the way anyone finds out is by
// authoring a level the editor called fine and the game aborts on. So rather
// than relying on care, every rule gets a deliberately broken level and both
// implementations are asked about it.
//
// Each case mutates tools/editor/testdata/base.json in one specific way, then:
//
//   * the editor's validator must produce an error with the expected rule,
//     naming the expected segment and side;
//   * the game must abort loading it, and its [FATAL] line must name the same
//     segment and side.
//
// Run from the repository root, after a Linux debug build:
//   node tools/editor/tests/conformance.mjs
//
// Two families of case are deliberately one-sided, and say so in their output:
//
//   * `editorOnly` rules (overlap, unreachable) have no counterpart in the
//     game. The assertion is inverted: the editor must warn AND the game must
//     load the level without complaint, which is the whole reason the check
//     exists here.
//   * `surface-open` is checked by --validate-assets, which only ever walks
//     assets/levels/. Pointing it at a broken file would mean copying that
//     file into the shipped asset directory, where an interrupted run would
//     leave something behind that makes every later game start fatal. Not
//     worth it: closure parity is instead evidenced by the shipped levels,
//     where this script checks the editor reproduces the game's exact
//     V-E+F and triangle counts.

import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { parseLevel, serializeLevel } from '../src/level.js';
import { emitJson } from '../src/json_emit.js';
import { EDITOR_ONLY_RULES, validateLevel } from '../src/validate.js';

const GAME = process.env.DESCENT_BIN ?? 'build/Debug/descent';
const BASE = 'tools/editor/testdata/base.json';

// What the game printed for the two shipped levels, from --validate-assets.
// If the editor's closure port drifts, these stop matching.
const SHIPPED = [
    { path: 'assets/levels/mine01.json', segments: 34, walls: 244, alt: 81, sides: 204, welded: 124, edges: 366 },
    { path: 'assets/levels/test01.json', segments: 14, walls: 112, alt: 42, sides: 84, welded: 58, edges: 168 },
];

// Each case names the rule it should trip and where. `side` is the side name
// as both implementations spell it, because that is what the game's message
// contains and a numeric index is not.
const CASES = [
    {
        name: 'a segment references a vertex that does not exist',
        rule: 'vertex-range', segment: 0,
        mutate: (j) => { j.segments[0].verts[3] = 999; },
    },
    {
        name: 'a portal names a segment that does not exist',
        rule: 'portal-range', segment: 0, side: 'right',
        mutate: (j) => { j.segments[0].sides[2].portal = 99; },
    },
    {
        name: 'a portal opens into itself',
        rule: 'portal-self', segment: 0, side: 'right',
        mutate: (j) => { j.segments[0].sides[2].portal = 0; },
    },
    {
        name: 'a portal is not reciprocated',
        rule: 'portal-reciprocity', segment: 0, side: 'right',
        mutate: (j) => { j.segments[1].sides[0].portal = 2; },
    },
    {
        name: 'two sides of a portal do not share their four vertices',
        rule: 'portal-vertices', segment: 0, side: 'right',
        mutate: (j) => { j.segments[1].verts[0] = 0; },
    },
    {
        name: 'the player start is in a segment that does not exist',
        rule: 'start-segment',
        mutate: (j) => { j.start.segment = 7; },
    },
    {
        name: 'a door is on a solid wall',
        rule: 'door-on-wall', segment: 0, side: 'top',
        mutate: (j) => { j.doors = [{ segment: 0, side: 1, key: 'none' }]; },
    },
    {
        name: 'a door is in a segment that does not exist',
        rule: 'door-segment',
        mutate: (j) => { j.doors = [{ segment: 9, side: 2, key: 'none' }]; },
    },
    {
        name: 'a door is on a side that is not one of the six',
        rule: 'door-side',
        mutate: (j) => { j.doors = [{ segment: 0, side: 9, key: 'none' }]; },
    },
    {
        name: 'a pickup is outside the mine',
        rule: 'pickup-outside',
        mutate: (j) => { j.pickups = [{ kind: 'shield', position: [999.0, 999.0, 999.0] }]; },
    },
    {
        name: 'an object is outside the mine',
        rule: 'object-outside',
        mutate: (j) => { j.objects = [{ type: 'pillar', position: [999.0, 999.0, 999.0] }]; },
    },
    {
        name: 'a floating light is outside the mine',
        rule: 'ball-outside',
        mutate: (j) => { j.floatingLights = [{ position: [999.0, 999.0, 999.0] }]; },
    },
    {
        name: 'a swinging light is anchored outside the mine',
        rule: 'lamp-outside',
        mutate: (j) => { j.swingingLights = [{ anchor: [999.0, 999.0, 999.0] }]; },
    },
    {
        name: 'a swinging light has a cord longer than the mine',
        rule: 'lamp-cord-outside',
        // Anchored inside, but on a cord that puts the lamp well below the
        // floor - which is exactly the level that loads and then lights the
        // room underneath through solid rock, if nobody checks.
        mutate: (j) => {
            j.swingingLights = [{ anchor: j.segments[0].verts ? [5.0, 5.0, 5.0] : [0, 0, 0],
                                  cordLength: 400.0 }];
        },
    },
    {
        name: 'a segment has the wrong number of sides',
        rule: 'parse', segment: 0,
        mutate: (j) => { j.segments[0].sides.pop(); },
    },
    {
        name: 'a segment has the wrong number of vertices',
        rule: 'parse', segment: 0,
        mutate: (j) => { j.segments[0].verts.pop(); },
    },
    {
        name: 'a segment nothing can reach from the player start',
        rule: 'segment-unreachable', segment: 3, editorOnly: true,
        // A closed box of its own, well away from the mine. Every side is a
        // wall, so the surface stays closed and the game has no complaint.
        mutate: (j) => detachedBox(j, 200, 220),
    },
    {
        name: 'two segments in the same place',
        rule: 'segment-overlap', segment: 0, editorOnly: true,
        // The same detached box, but sitting inside segment 0. Still closed,
        // still loads; findSegment now answers with whichever it tests first.
        mutate: (j) => detachedBox(j, 5, 15),
    },
];

// Appends a segment that touches nothing: eight fresh vertices, six walls.
// Nothing in the format forbids it and nothing in the game notices.
function detachedBox(j, lo, hi) {
    const base = j.vertices.length;
    for (const [x, y, z] of [[lo, lo, lo], [hi, lo, lo], [hi, hi, lo], [lo, hi, lo],
                             [lo, lo, hi], [hi, lo, hi], [hi, hi, hi], [lo, hi, hi]]) {
        j.vertices.push([x, y, z]);
    }
    j.segments.push({
        verts: [0, 1, 2, 3, 4, 5, 6, 7].map((k) => base + k),
        sides: [0, 1, 2, 3, 4, 5].map(() => ({ texture: 0 })),
    });
}

let failures = 0;
const report = (ok, label, detail = '') => {
    if (!ok) ++failures;
    console.log(`${ok ? 'ok  ' : 'FAIL'}  ${label}${detail ? `\n        ${detail}` : ''}`);
};

// --- closure parity on the shipped levels ----------------------------------

for (const want of SHIPPED) {
    const level = parseLevel(fs.readFileSync(want.path, 'utf8'), want.path);
    const { closure } = validateLevel(level);
    let alt = 0;
    let sides = 0;
    for (const seg of level.segments) for (const sd of seg.sides) { ++sides; if (sd.altDiagonal) ++alt; }

    const got = { segments: level.segments.length, walls: level.walls.length, alt, sides,
                  welded: closure.weldedVertices, edges: closure.edges };
    const same = Object.keys(want).filter((k) => k !== 'path').every((k) => got[k] === want[k]);
    report(same, `${want.path}: matches the game's counts`,
           same ? '' : `expected ${JSON.stringify(want)}, got ${JSON.stringify(got)}`);

    const euler = closure.weldedVertices - closure.edges + closure.triangles;
    report(closure.closed && euler === 2, `${want.path}: wall surface closed (V-E+F = ${euler})`);
}

// --- byte-exact round trip --------------------------------------------------

for (const want of SHIPPED) {
    const src = fs.readFileSync(want.path, 'utf8');
    const out = emitJson(serializeLevel(parseLevel(src, want.path)));
    report(out === src, `${want.path}: open and save is a no-op`,
           out === src ? '' : `${src.length} bytes in, ${out.length} bytes out`);
}

// --- one broken level per rule ---------------------------------------------

const haveGame = fs.existsSync(GAME);
if (!haveGame) {
    console.log(`\n  note: ${GAME} not found, so the game half of each case is skipped.`);
    console.log('        Build it with: cmake --preset linux && cmake --build --preset linux-debug\n');
}

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'descent-conformance-'));
const baseText = fs.readFileSync(BASE, 'utf8');

// Runs the game's own loader over a level file. --world-test loads and
// validates without a window or GL; its own PASS/FAIL is about whether the
// level is any good, which is a different question, so only the abort matters.
function loadWithGame(file) {
    try {
        const out = execFileSync(GAME, ['--world-test', '--level', file],
                                 { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
        return { aborted: false, text: out };
    } catch (e) {
        const text = `${e.stdout ?? ''}${e.stderr ?? ''}`;
        return { aborted: /\[FATAL\]/.test(text), text };
    }
}

for (const testCase of CASES) {
    const j = JSON.parse(baseText);
    testCase.mutate(j);
    const file = path.join(tmp, `${testCase.rule}-${CASES.indexOf(testCase)}.json`);
    fs.writeFileSync(file, JSON.stringify(j, null, 2));

    // The editor's answer. A shape the parser cannot even read is its own kind
    // of rejection and is expected for the `parse` cases.
    let editorRejected = false;
    let editorDetail = '';
    try {
        const result = validateLevel(parseLevel(fs.readFileSync(file, 'utf8'), file));
        const wanted = result.findings.filter((f) => f.rule === testCase.rule);
        const placed = wanted.filter((f) =>
            (testCase.segment === undefined || f.segment === testCase.segment));
        editorRejected = placed.length > 0;
        editorDetail = editorRejected ? '' :
            `findings: ${JSON.stringify(result.findings.map((f) => [f.rule, f.segment ?? null]))}`;
        if (testCase.editorOnly && editorRejected) {
            editorRejected = placed.every((f) => f.severity === 'warning') &&
                             EDITOR_ONLY_RULES.has(testCase.rule);
        }
    } catch (e) {
        editorRejected = testCase.rule === 'parse';
        editorDetail = editorRejected ? '' : `parser threw instead: ${e.message}`;
    }

    report(editorRejected, `editor rejects: ${testCase.name}`, editorDetail);

    if (!haveGame) continue;

    const game = loadWithGame(file);
    if (testCase.editorOnly) {
        // The point of an editor-only rule is that the game is happy with it.
        report(!game.aborted, `game is silent about: ${testCase.name}`,
               game.aborted ? 'the game aborted, so this rule is not editor-only after all' : '');
        continue;
    }

    if (!game.aborted) {
        report(false, `game rejects: ${testCase.name}`, 'the game loaded it without complaint');
        continue;
    }

    // Both aborted. Do they blame the same place?
    //
    // log::fatal writes the message as an [ERROR] line and then a bare
    // "[FATAL] aborting", so the detail is the last error before the abort.
    const lines = game.text.split('\n');
    const fatal = lines.slice(0, lines.findIndex((l) => l.includes('[FATAL]')))
                       .filter((l) => l.includes('[ERROR]')).pop() ?? '';
    // "segment 3" in most messages, "segments 0 (right) and 1 (left)" in the
    // one that names both ends of a portal; the side appears bare or in
    // parentheses. Matching the index and the side name is the assertion -
    // comparing whole strings would make every reworded log line a failure.
    const blames = testCase.segment === undefined ||
                   new RegExp(`segments? ${testCase.segment}\\b`).test(fatal);
    const agrees = blames && (testCase.side === undefined || fatal.includes(testCase.side));
    report(agrees, `game rejects and blames the same place: ${testCase.name}`,
           agrees ? '' : `expected segment ${testCase.segment} side ${testCase.side ?? '-'} in:\n        ${fatal.trim()}`);
}

fs.rmSync(tmp, { recursive: true, force: true });

console.log(`\n${failures === 0 ? 'conformance: PASS' : `conformance: FAIL (${failures})`}`);
process.exit(failures === 0 ? 0 : 1);
