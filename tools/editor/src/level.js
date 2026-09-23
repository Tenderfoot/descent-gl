// Reading and writing assets/levels/*.json.
//
// The writer must produce what saveLevel() in src/game/level.cpp produces,
// field for field: `texture` only on walls, `door` only on portal sides,
// optional fields omitted at their defaults. A writer that is merely
// *equivalent* turns every save into an unreviewable diff.
//
// One deliberate difference: `comment` is preserved. saveLevel drops it, which
// silently deletes the prose at the top of mine01.json the first time the
// in-game light editor's Save button is pressed.

import { BALL_COLOR, DEFAULTS, KEY_NAMES, LAMP_COLOR, PICKUP_NAMES, SIDE_COUNT } from './format.js';
import { rewireDoors, rewireGrates } from './entities.js';
import { findSegment, rebuildDerived } from './geometry.js';

const vec3 = (j) => {
    if (!Array.isArray(j) || j.length !== 3) throw new Error('expected [x, y, z]');
    return [Number(j[0]), Number(j[1]), Number(j[2])];
};

// The file stores quaternions as [w, x, y, z]. THREE.Quaternion is (x, y, z, w).
// Everything inside the editor uses the file's order; the single conversion
// lives in view.js and nowhere else.
const quat = (j, fallback) => (Array.isArray(j) && j.length === 4 ? j.map(Number) : fallback.slice());

export function parseLevel(json, path = '<memory>') {
    const j = typeof json === 'string' ? JSON.parse(json) : json;

    const level = {
        path,
        // The two shipped levels order their keys differently - test01 has a
        // spot light that names its segment before its direction, mine01's
        // lights have no direction at all. Keeping each object's source order
        // is what makes "open and save" a no-op diff instead of a reshuffle.
        keyOrder: Object.keys(j),
        name: j.name ?? path,
        comment: j.comment ?? null,
        script: j.script ?? null,
        vertices: [],
        segments: [],
        lights: [],
        objects: [],
        doors: [],
        grates: [],
        pickups: [],
        floatingLights: [],
        swingingLights: [],
        exitTrigger: null,
        start: null,
    };

    if (!Array.isArray(j.vertices)) throw new Error('level has no "vertices" array');
    for (const v of j.vertices) level.vertices.push(vec3(v));

    if (!Array.isArray(j.segments)) throw new Error('level has no "segments" array');
    for (let si = 0; si < j.segments.length; ++si) {
        const sj = j.segments[si];
        if (!Array.isArray(sj.verts) || sj.verts.length !== 8) {
            throw new Error(`segment ${si} has ${sj.verts?.length ?? 0} vertices, expected 8`);
        }
        if (!Array.isArray(sj.sides) || sj.sides.length !== SIDE_COUNT) {
            throw new Error(`segment ${si} has ${sj.sides?.length ?? 0} sides, expected ${SIDE_COUNT} ` +
                            '(left, top, right, bottom, back, front)');
        }
        level.segments.push({
            verts: sj.verts.map(Number),
            sides: sj.sides.map((side) => ({
                neighbour: side.portal ?? -1,
                door: side.door ?? -1,
                // Derived from level.grates like `door`; see rewireGrates.
                grate: -1,
                texture: side.texture ?? 0,
                emissive: Number(side.emissive ?? 0),
                // null, not a filled array: the loader distinguishes "authored"
                // from "left to the planar default" and so must the writer.
                uv: Array.isArray(side.uv) ? side.uv.map((p) => [Number(p[0]), Number(p[1])]) : null,
            })),
        });
    }

    for (const lj of j.lights ?? []) {
        level.lights.push({
            keyOrder: Object.keys(lj),
            position: vec3(lj.position),
            radius: lj.radius ?? DEFAULTS.lightRadius,
            color: lj.color ? vec3(lj.color) : [1, 1, 1],
            intensity: lj.intensity ?? DEFAULTS.lightIntensity,
            type: lj.type === 'spot' ? 'spot' : 'point',
            direction: lj.direction ? vec3(lj.direction) : [0, -1, 0],
            coneDegrees: lj.cone ?? DEFAULTS.lightCone,
            fixtureSize: lj.fixtureSize ?? DEFAULTS.lightFixtureSize,
            segment: lj.segment ?? -1,
        });
    }

    for (const dj of j.doors ?? []) {
        level.doors.push({
            keyOrder: Object.keys(dj),
            segment: dj.segment,
            side: dj.side,
            key: KEY_NAMES.indexOf(dj.key ?? 'none'),   // -1 is KeyColor::None
            hatch: dj.hatch === true,                   // sealed until the reactor goes
            openDistance: dj.openDistance ?? DEFAULTS.doorOpenDistance,
            openTime: dj.openTime ?? DEFAULTS.doorOpenTime,
            closeDelay: dj.closeDelay ?? DEFAULTS.doorCloseDelay,
        });
    }

    for (const gj of j.grates ?? []) {
        level.grates.push({
            keyOrder: Object.keys(gj),
            segment: gj.segment,
            side: gj.side,
            texture: gj.texture,
        });
    }

    for (const pj of j.pickups ?? []) {
        const kind = PICKUP_NAMES.indexOf(pj.kind);
        if (kind < 0) throw new Error(`pickup has unknown kind '${pj.kind}'`);
        level.pickups.push({
            keyOrder: Object.keys(pj),
            kind,
            position: vec3(pj.position),
            segment: pj.segment ?? -1,
        });
    }

    for (const fj of j.floatingLights ?? []) {
        level.floatingLights.push({
            keyOrder: Object.keys(fj),
            position: vec3(fj.position),
            radius: fj.radius ?? DEFAULTS.ballRadius,
            color: fj.color ? vec3(fj.color) : BALL_COLOR.slice(),
            lightRadius: fj.lightRadius ?? DEFAULTS.ballLightRadius,
            lightIntensity: fj.lightIntensity ?? DEFAULTS.ballLightIntensity,
            restitution: fj.restitution ?? DEFAULTS.ballRestitution,
            mass: fj.mass ?? DEFAULTS.ballMass,
            drift: fj.drift ?? DEFAULTS.ballDrift,
            segment: fj.segment ?? -1,
        });
    }

    // The file calls a swinging light's point `anchor`, because that is what it
    // is: the lamp hangs a cord's length below it. Inside the editor it is
    // `position` like every other entity's, so selecting, dragging, segment
    // resolution and validation are written once rather than once per kind.
    // serializeLevel puts the name back.
    for (const sj of j.swingingLights ?? []) {
        level.swingingLights.push({
            keyOrder: Object.keys(sj),
            position: vec3(sj.anchor),
            cordLength: sj.cordLength ?? DEFAULTS.lampCordLength,
            amplitude: sj.amplitude ?? DEFAULTS.lampAmplitude,
            rate: sj.rate ?? DEFAULTS.lampRate,
            phase: sj.phase ?? DEFAULTS.lampPhase,
            swingAxis: sj.swingAxis ? vec3(sj.swingAxis) : [1, 0, 0],
            color: sj.color ? vec3(sj.color) : LAMP_COLOR.slice(),
            lightRadius: sj.lightRadius ?? DEFAULTS.lampLightRadius,
            lightIntensity: sj.lightIntensity ?? DEFAULTS.lampLightIntensity,
            coneDegrees: sj.coneDegrees ?? DEFAULTS.lampCone,
            segment: sj.segment ?? -1,
        });
    }

    if (j.exit) {
        level.exitTrigger = {
            keyOrder: Object.keys(j.exit),
            position: vec3(j.exit.position),
            radius: j.exit.radius ?? DEFAULTS.exitRadius,
            segment: j.exit.segment ?? -1,
        };
    }

    for (const oj of j.objects ?? []) {
        level.objects.push({
            keyOrder: Object.keys(oj),
            type: String(oj.type),
            position: vec3(oj.position),
            orientation: quat(oj.orientation, [1, 0, 0, 0]),
            scale: oj.scale ?? DEFAULTS.objectScale,
            segment: oj.segment ?? -1,
            health: oj.health ?? DEFAULTS.objectHealth,
        });
    }

    if (!j.start) throw new Error('level has no "start"');
    level.start = {
        keyOrder: Object.keys(j.start),
        segment: j.start.segment ?? 0,
        position: vec3(j.start.position),
        orientation: quat(j.start.orientation, [1, 0, 0, 0]),
    };

    rebuildDerived(level);
    // A side's `door` is derived, not authored: loadLevel reads whatever the
    // file said and then overwrites it from the doors array, on both ends of
    // the portal. The shipped levels carry no `door` on any side at all, so
    // without this step the editor believed a portal with a door on it was
    // empty - and offered to add a second one.
    rewireDoors(level);
    rewireGrates(level);
    resolveSegments(level);
    return level;
}

// The loader fills in a missing or out-of-range `segment` for lights, pickups,
// objects and the exit by searching for the one that contains the point. Doing
// the same here means the editor shows the same association the game will use,
// and that a save writes the resolved value rather than the stale one.
export function resolveSegments(level) {
    const n = level.segments.length;
    const fix = (e) => {
        if (e.segment < 0 || e.segment >= n) e.segment = findSegment(level, e.position, e.segment);
    };
    level.lights.forEach(fix);
    level.pickups.forEach(fix);
    level.objects.forEach(fix);
    level.floatingLights.forEach(fix);
    level.swingingLights.forEach(fix);
    if (level.exitTrigger) fix(level.exitTrigger);
}

// Matches saveLevel(). Numbers go through a round-trip-stable formatter: the
// file is read as JSON and written as JSON, so 7.0 must not come back as 7.
export function serializeLevel(level) {
    const j = {};
    j.name = level.name;
    if (level.comment !== null && level.comment !== undefined) j.comment = level.comment;
    if (level.script !== null && level.script !== undefined) j.script = level.script;

    j.vertices = level.vertices.map((v) => [v[0], v[1], v[2]]);

    j.segments = level.segments.map((seg) => ({
        verts: seg.verts.slice(),
        sides: seg.sides.map((sd) => {
            const side = {};
            if (sd.neighbour >= 0) {
                side.portal = sd.neighbour;
                // saveLevel writes `door` here; this does not. It is derived -
                // the loader rewires every side from the doors array whatever
                // the file says - and writing it would put a second, silent
                // copy of the door graph in the file for the two to disagree
                // about. The shipped levels do not carry it, so omitting it is
                // also what keeps open-and-save byte-identical now that the
                // editor resolves doors at load.
            } else {
                side.texture = sd.texture;
                // Omitted at the default, matching saveLevel: a level that
                // emits nowhere writes exactly what it did before sides could.
                if (sd.emissive > 0) side.emissive = sd.emissive;
            }
            if (sd.uv) side.uv = sd.uv.map((p) => [p[0], p[1]]);
            return side;
        }),
    }));

    if (level.doors.length) {
        j.doors = level.doors.map((d) => {
            const dj = { segment: d.segment, side: d.side, key: d.key >= 0 ? KEY_NAMES[d.key] : 'none' };
            // Only when set, matching saveLevel: an ordinary door says nothing
            // about the reactor and a level full of "hatch": false would be
            // noise in every diff.
            if (d.hatch) dj.hatch = true;
            // saveLevel always writes these three; the shipped levels leave
            // them out and take the defaults. Omitting them keeps a door that
            // nobody has tuned looking untouched.
            if (d.openDistance !== DEFAULTS.doorOpenDistance) dj.openDistance = d.openDistance;
            if (d.openTime !== DEFAULTS.doorOpenTime) dj.openTime = d.openTime;
            if (d.closeDelay !== DEFAULTS.doorCloseDelay) dj.closeDelay = d.closeDelay;
            return reorder(dj, d.keyOrder);
        });
    }

    // Every field always, matching saveLevel: a grate has no defaults to omit.
    if (level.grates.length) {
        j.grates = level.grates.map((g) => reorder(
            { segment: g.segment, side: g.side, texture: g.texture }, g.keyOrder));
    }

    if (level.lights.length) {
        j.lights = level.lights.map((l) => {
            const lj = {
                position: l.position.slice(),
                radius: l.radius,
                color: l.color.slice(),
                intensity: l.intensity,
                type: l.type,
            };
            if (l.type === 'spot') {
                lj.direction = l.direction.slice();
                lj.cone = l.coneDegrees;
            }
            // Omitted when there is no fixture, matching saveLevel: almost no
            // light has one, and a zero on every light would be noise.
            if (l.fixtureSize !== DEFAULTS.lightFixtureSize) lj.fixtureSize = l.fixtureSize;
            lj.segment = l.segment;
            return reorder(lj, l.keyOrder);
        });
    }

    if (level.objects.length) {
        j.objects = level.objects.map((o) => {
            const oj = { type: o.type, position: o.position.slice(), orientation: o.orientation.slice() };
            if (o.scale !== DEFAULTS.objectScale) oj.scale = o.scale;
            if (o.health > DEFAULTS.objectHealth) oj.health = o.health;
            oj.segment = o.segment;
            return reorder(oj, o.keyOrder);
        });
    }

    if (level.floatingLights.length) {
        j.floatingLights = level.floatingLights.map((b) => reorder({
            position: b.position.slice(),
            segment: b.segment,
            radius: b.radius,
            color: b.color.slice(),
            lightRadius: b.lightRadius,
            lightIntensity: b.lightIntensity,
            restitution: b.restitution,
            mass: b.mass,
            drift: b.drift,
        }, b.keyOrder));
    }

    if (level.swingingLights.length) {
        j.swingingLights = level.swingingLights.map((l) => reorder({
            anchor: l.position.slice(),
            segment: l.segment,
            cordLength: l.cordLength,
            amplitude: l.amplitude,
            rate: l.rate,
            phase: l.phase,
            swingAxis: l.swingAxis.slice(),
            color: l.color.slice(),
            lightRadius: l.lightRadius,
            lightIntensity: l.lightIntensity,
            coneDegrees: l.coneDegrees,
        }, l.keyOrder));
    }

    if (level.pickups.length) {
        j.pickups = level.pickups.map((p) => reorder({
            kind: PICKUP_NAMES[p.kind],
            position: p.position.slice(),
            segment: p.segment,
        }, p.keyOrder));
    }

    if (level.exitTrigger) {
        j.exit = reorder({
            position: level.exitTrigger.position.slice(),
            radius: level.exitTrigger.radius,
            segment: level.exitTrigger.segment,
        }, level.exitTrigger.keyOrder);
    }

    j.start = reorder({
        segment: level.start.segment,
        position: level.start.position.slice(),
        orientation: level.start.orientation.slice(),
    }, level.start.keyOrder);

    return reorder(j, level.keyOrder);
}

// Keys the source object had, in its order, then anything new this save added.
// A field the editor set for the first time lands at the end rather than in
// some canonical slot, which is the smaller diff.
function reorder(j, keyOrder) {
    if (!keyOrder) return j;
    const out = {};
    for (const k of keyOrder) if (k in j) out[k] = j[k];
    for (const k of Object.keys(j)) if (!(k in out)) out[k] = j[k];
    return out;
}
