// Making a level that does not exist yet.
//
// The editor can only claim to author a mine if a mine can start from nothing,
// so this checks the starter level is one the game will actually load and one
// extrude can grow - not merely an object that passes the editor's own checks.
//
//   node tools/editor/tests/create.mjs

import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { checkName, newLevel } from '../src/create.js';
import { emitJson } from '../src/json_emit.js';
import { parseLevel, serializeLevel } from '../src/level.js';
import { extrudeSide } from '../src/ops.js';
import { validateLevel } from '../src/validate.js';

const GAME = process.env.DESCENT_BIN ?? 'build/Debug/descent';

let failures = 0;
const report = (ok, label, detail = '') => {
    if (!ok) ++failures;
    console.log(`${ok ? 'ok  ' : 'FAIL'}  ${label}${detail ? `\n        ${detail}` : ''}`);
};

// --- the starter level ------------------------------------------------------

{
    const level = newLevel('starter');
    const { findings, closure } = validateLevel(level);

    report(findings.length === 0, 'a new level has nothing wrong with it',
           findings.map((f) => `${f.severity}:${f.rule}`).join(', '));
    report(level.segments.length === 1 && level.vertices.length === 8 && level.walls.length === 12,
           'one segment, eight vertices, twelve wall triangles',
           `${level.segments.length}/${level.vertices.length}/${level.walls.length}`);

    const euler = closure.weldedVertices - closure.edges + closure.triangles;
    report(closure.closed && euler === 2, 'and a closed wall surface', `V-E+F = ${euler}`);
    report(level.start.segment === 0, 'with the player start inside it');
    report(level.lights.length === 1 && level.lights[0].segment === 0,
           'and one light, so the room is not simply black');
}

{
    // The starter has to be growable, not just valid: every side is a wall, so
    // extrude must work in all six directions from it.
    for (let side = 0; side < 6; ++side) {
        const level = newLevel('starter');
        const created = extrudeSide(level, 0, side);
        const errors = validateLevel(level).findings.filter((f) => f.severity === 'error');
        report(created === 1 && errors.length === 0,
               `a new level can be extruded from side ${side}`,
               errors.map((f) => f.rule).join(', '));
    }
}

{
    const level = newLevel('starter');
    const text = emitJson(serializeLevel(level));
    report(text === emitJson(serializeLevel(parseLevel(text, 'starter'))),
           'a new level round-trips through the writer');
}

// --- names ------------------------------------------------------------------

{
    const existing = ['mine01', 'test01'];
    report(checkName('mine02', existing) === null, 'a fresh name is accepted');
    report(checkName('mine01', existing) !== null, 'an existing name is refused');
    report(checkName('', existing) !== null, 'an empty name is refused');
    report(checkName('../../etc/passwd', existing) !== null, 'a path is refused');
    report(checkName('a b', existing) !== null, 'a name with a space is refused');
    report(checkName('-nope', existing) !== null, 'a name starting with a dash is refused');
    report(checkName('a'.repeat(65), existing) !== null, 'an absurdly long name is refused');
}

// --- the game agrees --------------------------------------------------------

if (fs.existsSync(GAME)) {
    const level = newLevel('starter');
    extrudeSide(level, 0, 1);

    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'descent-create-'));
    const file = path.join(tmp, 'starter.json');
    fs.writeFileSync(file, emitJson(serializeLevel(level)));

    let output = '';
    try {
        output = execFileSync(GAME, ['--world-test', '--level', file],
                              { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
    } catch (e) {
        output = `${e.stdout ?? ''}${e.stderr ?? ''}`;
    }
    report(!/\[FATAL\]/.test(output), 'the game loads a level made from scratch and grown once',
           (output.split('\n').filter((l) => l.includes('[ERROR]')).pop() ?? '').trim());

    // What it objects to should only ever be "this level is too small to
    // exercise what I test" - never a structural complaint.
    const notes = output.split('\n').filter((l) => l.includes('[ERROR]'))
                        .map((l) => l.split('[ERROR] ')[1].trim());
    const onlySize = notes.every((n) =>
        /no segment is dark|portal culling is doing nothing|silhouette edge/.test(n));
    report(onlySize, 'and only objects that it is too small to exercise the renderer',
           notes.join(' | ') || '(nothing)');

    fs.rmSync(tmp, { recursive: true, force: true });
} else {
    console.log(`\n  note: ${GAME} not found, so the game half is skipped.\n`);
}

console.log(`\n${failures === 0 ? 'create: PASS' : `create: FAIL (${failures})`}`);
process.exit(failures === 0 ? 0 : 1);
