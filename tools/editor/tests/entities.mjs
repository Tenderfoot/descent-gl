// Placing and editing the things inside a mine.
//
// The rule these exist to protect is that an entity's cached `segment` says
// which segment actually contains it. The loader recomputes a segment that is
// missing or out of range but TRUSTS one that is merely wrong, so a light
// dragged across a portal without re-resolving lights the wrong room and
// nothing anywhere complains. Most of what follows is that one rule.
//
//   node tools/editor/tests/entities.mjs

import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { KEY_NAMES, PICKUP_NAMES, SIDE_NAMES } from '../src/format.js';
import { findSegment } from '../src/geometry.js';
import { emitJson } from '../src/json_emit.js';
import { parseLevel, serializeLevel } from '../src/level.js';
import { parseObj } from '../src/obj.js';
import { extrudeSide } from '../src/ops.js';
import {
    addDoor, addLight, addObject, addPickup, availableKeys, moveEntity, removeEntity,
    rewireDoors, segmentCentre, setExit,
} from '../src/entities.js';
import { validateLevel } from '../src/validate.js';

const GAME = process.env.DESCENT_BIN ?? 'build/Debug/descent';

let failures = 0;
const report = (ok, label, detail = '') => {
    if (!ok) ++failures;
    console.log(`${ok ? 'ok  ' : 'FAIL'}  ${label}${detail ? `\n        ${detail}` : ''}`);
};

const fresh = (file = 'tools/editor/testdata/base.json') =>
    parseLevel(fs.readFileSync(file, 'utf8'), file);
const clean = (level) => validateLevel(level).findings.filter((f) => f.severity === 'error');

// --- the OBJ parser against the game's own counts ---------------------------

// From `descent --validate-assets`. If the parser drifts, a robot previews as
// the wrong shape and placing it is guesswork again.
const MODEL_TRIANGLES = {
    brute: 72, drone: 20, gunner: 48, pillar: 28, reactor: 76, ship: 22,
};
for (const [name, triangles] of Object.entries(MODEL_TRIANGLES)) {
    const model = parseObj(fs.readFileSync(`assets/models/${name}.obj`, 'utf8'));
    report(model.count / 3 === triangles, `${name}.obj parses to the game's ${triangles} triangles`,
           `got ${model.count / 3}`);
}

// --- placing ----------------------------------------------------------------

{
    const level = fresh();
    const index = addLight(level, segmentCentre(level, 2));
    report(level.lights[index].segment === 2 && clean(level).length === 0,
           'a light placed in a segment knows which segment it is in',
           `segment = ${level.lights[index].segment}`);
}

{
    const level = fresh();
    const index = addPickup(level, PICKUP_NAMES.indexOf('key_blue'), segmentCentre(level, 1));
    report(level.pickups[index].segment === 1 && clean(level).length === 0,
           'so does a pickup');
    report(availableKeys(level).has(KEY_NAMES.indexOf('blue')),
           'and the door editor can see which keys the level actually has');
}

{
    const level = fresh();
    const index = addObject(level, 'drone', segmentCentre(level, 0));
    report(level.objects[index].segment === 0 && clean(level).length === 0, 'and an object');
    let refused = false;
    try { addObject(level, '', [0, 0, 0]); } catch { refused = true; }
    report(refused, 'an object without a model name is refused');
}

{
    const level = fresh();
    setExit(level, segmentCentre(level, 2));
    report(level.exitTrigger.segment === 2 && clean(level).length === 0,
           'and the exit trigger');
}

// --- the rule that matters --------------------------------------------------

{
    // Drag a light from one segment to another. If `segment` is not
    // re-resolved the level still validates and the light lights the wrong
    // room - which is the failure this whole module exists to prevent.
    const level = fresh();
    const index = addLight(level, segmentCentre(level, 0));
    report(level.lights[index].segment === 0, 'a light starts in the segment it was placed in');

    moveEntity(level, 'light', index, segmentCentre(level, 2));
    report(level.lights[index].segment === 2,
           'and moving it across two portals re-resolves the segment it is in',
           `segment = ${level.lights[index].segment}, expected 2`);

    // The same, checked against the loader's own answer rather than against 2.
    const expected = findSegment(level, level.lights[index].position, -1);
    report(level.lights[index].segment === expected,
           'and agrees with findSegment, which is what the loader would use');
}

{
    const level = fresh();
    moveEntity(level, 'start', 0, segmentCentre(level, 2));
    report(level.start.segment === 2 && clean(level).length === 0,
           'moving the player start re-resolves its segment too');
}

{
    // Out of the mine entirely. The loader is fatal on a pickup that is
    // nowhere, so the editor has to report it rather than write it.
    const level = fresh();
    const index = addPickup(level, 0, [999, 999, 999]);
    const errors = clean(level);
    report(level.pickups[index].segment === -1 &&
           errors.some((f) => f.rule === 'pickup-outside'),
           'a pickup placed outside the mine is reported, not silently written');
}

// --- doors ------------------------------------------------------------------

{
    const level = fresh();
    const di = addDoor(level, 0, 2, KEY_NAMES.indexOf('blue'));
    const near = level.segments[0].sides[2];
    const far = level.segments[1].sides[0];
    report(near.door === di && far.door === di && clean(level).length === 0,
           'a door is wired onto both sides of its portal',
           `near=${near.door} far=${far.door}`);
}

{
    const level = fresh();
    let refused = false;
    let message = '';
    try { addDoor(level, 0, 1); } catch (e) { refused = true; message = e.message; }
    report(refused && /solid wall/.test(message),
           'a door on a solid wall is refused, with the reason the game would give');
}

{
    const level = fresh();
    addDoor(level, 0, 2);
    let refused = false;
    try { addDoor(level, 0, 2); } catch { refused = true; }
    report(refused, 'a portal cannot have two doors');
}

{
    // Removing the first of two doors must renumber the second everywhere,
    // or a side points at a door that is now a different door.
    const level = fresh();
    addDoor(level, 0, 2);
    const second = addDoor(level, 1, 2);
    removeEntity(level, 'door', 0);

    const sd = level.segments[1].sides[2];
    report(level.doors.length === 1 && sd.door === 0 && clean(level).length === 0,
           'removing a door renumbers the ones after it, on both sides',
           `doors=${level.doors.length} side.door=${sd.door}`);
    report(level.segments[0].sides[2].door === -1,
           'and the portal it was on no longer claims one');
}

// --- removal ----------------------------------------------------------------

{
    const level = fresh();
    addLight(level, segmentCentre(level, 0));
    const before = level.lights.length;
    removeEntity(level, 'light', 0);
    report(level.lights.length === before - 1 && clean(level).length === 0,
           'removing a light leaves the level valid');

    let refused = false;
    try { removeEntity(level, 'start', 0); } catch { refused = true; }
    report(refused, 'the player start cannot be removed - a level needs one');
}

// --- the game agrees --------------------------------------------------------

{
    const level = fresh();
    const room = extrudeSide(level, 2, 1);
    addLight(level, segmentCentre(level, room));
    addObject(level, 'drone', segmentCentre(level, room));
    // The loop needs something to blow up: --loop-test checks that the exit
    // stays shut until the reactor is gone, so a level without one fails on
    // "the level has a reactor" rather than on anything the editor did.
    addObject(level, 'reactor', segmentCentre(level, room));
    addPickup(level, PICKUP_NAMES.indexOf('key_blue'), segmentCentre(level, 1));
    addDoor(level, 0, 2, KEY_NAMES.indexOf('blue'));
    setExit(level, segmentCentre(level, 2));

    const errors = clean(level);
    report(errors.length === 0,
           'a level with a new room, light, robot, reactor, key, door and exit validates',
           errors.map((f) => f.message).join('; '));

    const text = emitJson(serializeLevel(level));
    report(text === emitJson(serializeLevel(parseLevel(text, 'edited'))),
           'and round-trips through the writer');

    // The key one: the door, the key colour and the segment fields all have to
    // mean the same thing to the game as they do here.
    const reloaded = parseLevel(text, 'edited');
    report(reloaded.doors.length === 1 && reloaded.doors[0].key === KEY_NAMES.indexOf('blue') &&
           reloaded.segments[0].sides[2].door === 0,
           'and the door survives the round trip with its key and its wiring');

    if (fs.existsSync(GAME)) {
        const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'descent-entities-'));
        const file = path.join(tmp, 'entities.json');
        fs.writeFileSync(file, text);
        let loaded = false;
        let detail = '';
        try {
            execFileSync(GAME, ['--world-test', '--level', file],
                         { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
            loaded = true;
        } catch (e) {
            const out = `${e.stdout ?? ''}${e.stderr ?? ''}`;
            loaded = !/\[FATAL\]/.test(out);
            detail = (out.split('\n').filter((l) => l.includes('[ERROR]')).pop() ?? '').trim();
        }
        report(loaded, 'and the game loads it without aborting', loaded ? '' : detail);

        // --loop-test is deliberately NOT used as the oracle here. Despite the
        // name it is mine01's regression test, not a general playability
        // check: src/game/loop_test.cpp asserts the level has exactly three
        // keyed doors and a gunner. A level with one door and no gunner is
        // neither skipped (it has a door and an exit) nor passing, so its
        // verdict says nothing about whether the editor did its job.
        //
        // What is worth asserting is the part that IS general: that the key
        // the editor placed is seen and collected, and that it opens the door
        // the editor put it against.
        let loop = '';
        try {
            loop = execFileSync(GAME, ['--loop-test', '--level', file],
                                { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
        } catch (e) {
            loop = `${e.stdout ?? ''}${e.stderr ?? ''}`;
        }
        const collected = /automap: key 0 seen=true collected=true/.test(loop);
        const pickups = loop.split('\n').find((l) => /pickups: /.test(l)) ?? '';
        report(collected && /1 of 1 collected and had an effect/.test(pickups),
               'and the game walks to the key the editor placed and collects it',
               `${pickups.trim()} | key seen and collected: ${collected}`);

        const mineOnly = loop.split('\n')
            .filter((l) => /FAILED/.test(l))
            .map((l) => l.replace(/^.*FAILED: /, ''));
        report(mineOnly.every((f) => /three keyed doors|a gunner/.test(f)),
               'and the only things it objects to are the ones written for mine01',
               mineOnly.join(' | ') || '(none)');
        fs.rmSync(tmp, { recursive: true, force: true });
    } else {
        console.log(`\n  note: ${GAME} not found, so the game half is skipped.\n`);
    }
}

console.log(`\n${failures === 0 ? 'entities: PASS' : `entities: FAIL (${failures})`}`);
process.exit(failures === 0 ? 0 : 1);
