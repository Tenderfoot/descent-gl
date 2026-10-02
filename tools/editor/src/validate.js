// Every structural rule the level format promises, checked the way the game
// checks it.
//
// Ported from validate() in src/game/level.cpp, the door and entity resolution
// that follows it in loadLevel(), and the closed-surface check in
// src/game/asset_check.cpp. The C++ is the authority: tools/editor/tests
// runs a corpus of deliberately broken levels through both and fails if they
// disagree about which segment and side is at fault.
//
// Two checks here have no counterpart in the C++, marked `editorOnly`. They
// catch things the game loads happily and then behaves strangely about, which
// is exactly the kind of bug an editor exists to prevent.

import { KEY_NAMES, PICKUP_NAMES, SIDE_COUNT, SIDE_NAMES, SIDE_VERTICES } from './format.js';
import { availableKeys, lampPosition } from './entities.js';
import { findSegment, insideSegment, sideCorners } from './geometry.js';

const finding = (severity, rule, message, where = {}) => ({ severity, rule, message, ...where });

// --- the loader's own rules ------------------------------------------------

function checkStructure(level, out) {
    const n = level.segments.length;

    for (let si = 0; si < n; ++si) {
        const seg = level.segments[si];

        for (const v of seg.verts) {
            if (v < 0 || v >= level.vertices.length) {
                out.push(finding('error', 'vertex-range',
                    `segment ${si} references vertex ${v}, which does not exist`, { segment: si }));
            }
        }

        for (let side = 0; side < SIDE_COUNT; ++side) {
            const sd = seg.sides[side];
            if (sd.neighbour < 0) continue;

            if (sd.neighbour >= n) {
                out.push(finding('error', 'portal-range',
                    `segment ${si} side ${SIDE_NAMES[side]} is a portal into segment ` +
                    `${sd.neighbour}, which does not exist`, { segment: si, side }));
                continue;
            }
            if (sd.neighbour === si) {
                out.push(finding('error', 'portal-self',
                    `segment ${si} side ${SIDE_NAMES[side]} is a portal into itself`,
                    { segment: si, side }));
                continue;
            }

            // Reciprocity. Checking only that the neighbour points back would
            // accept two segments joined through different faces, which walks
            // and renders fine right up until a light leaks through the seam.
            const other = level.segments[sd.neighbour];
            let matching = -1;
            for (let os = 0; os < SIDE_COUNT; ++os) {
                if (other.sides[os].neighbour === si) { matching = os; break; }
            }
            if (matching < 0) {
                out.push(finding('error', 'portal-reciprocity',
                    `segment ${si} side ${SIDE_NAMES[side]} opens into segment ${sd.neighbour}, ` +
                    'which does not open back', { segment: si, side }));
                continue;
            }

            const mine = SIDE_VERTICES[side].map((c) => seg.verts[c]).sort((a, b) => a - b);
            const theirs = SIDE_VERTICES[matching].map((c) => other.verts[c]).sort((a, b) => a - b);
            if (mine.join() !== theirs.join()) {
                out.push(finding('error', 'portal-vertices',
                    `portal between segments ${si} (${SIDE_NAMES[side]}) and ${sd.neighbour} ` +
                    `(${SIDE_NAMES[matching]}) does not share its four vertices: ` +
                    `[${mine.join(' ')}] vs [${theirs.join(' ')}]`, { segment: si, side }));
            }
        }
    }

    if (level.start.segment < 0 || level.start.segment >= n) {
        out.push(finding('error', 'start-segment',
            `player start is in segment ${level.start.segment}, which does not exist`));
    }
}

// Doors are resolved after the split in loadLevel, and each of these is fatal.
function checkDoors(level, out) {
    const n = level.segments.length;
    for (let di = 0; di < level.doors.length; ++di) {
        const door = level.doors[di];
        if (door.segment < 0 || door.segment >= n) {
            out.push(finding('error', 'door-segment',
                `door ${di} is in segment ${door.segment}, which does not exist`,
                { segment: door.segment }));
            continue;
        }
        if (door.side < 0 || door.side >= SIDE_COUNT) {
            out.push(finding('error', 'door-side',
                `door ${di} is on side ${door.side}, which is not one of the six`,
                { segment: door.segment }));
            continue;
        }
        if (level.segments[door.segment].sides[door.side].neighbour < 0) {
            out.push(finding('error', 'door-on-wall',
                `door ${di} is on segment ${door.segment} side ${SIDE_NAMES[door.side]}, which is ` +
                'a solid wall. A door has to be a portal - there must be somewhere for it to ' +
                'open into.', { segment: door.segment, side: door.side }));
        }
    }
}

// Grates are resolved after doors in loadLevel, and each of these is fatal.
function checkGrates(level, out) {
    const n = level.segments.length;
    const taken = new Map();   // "segment/side" of both ends -> grate index
    for (let gi = 0; gi < level.grates.length; ++gi) {
        const grate = level.grates[gi];
        if (grate.segment < 0 || grate.segment >= n) {
            out.push(finding('error', 'grate-segment',
                `grate ${gi} is in segment ${grate.segment}, which does not exist`,
                { segment: grate.segment }));
            continue;
        }
        if (grate.side < 0 || grate.side >= SIDE_COUNT) {
            out.push(finding('error', 'grate-side',
                `grate ${gi} is on side ${grate.side}, which is not one of the six`,
                { segment: grate.segment }));
            continue;
        }
        const where = { segment: grate.segment, side: grate.side };
        const sd = level.segments[grate.segment].sides[grate.side];
        if (sd.neighbour < 0) {
            out.push(finding('error', 'grate-on-wall',
                `grate ${gi} is on segment ${grate.segment} side ${SIDE_NAMES[grate.side]}, which ` +
                'is a solid wall. A grate fills a portal - there must be something on the other ' +
                'side of it.', where));
            continue;
        }
        if (sd.door >= 0) {
            out.push(finding('error', 'grate-on-door',
                `grate ${gi} is on segment ${grate.segment} side ${SIDE_NAMES[grate.side]}, which ` +
                `already holds door ${sd.door}`, where));
            continue;
        }

        // Either end of the portal counts as the same opening, so a second
        // grate naming the far side is caught as well as one naming this side.
        const here = `${grate.segment}/${grate.side}`;
        if (taken.has(here)) {
            out.push(finding('error', 'grate-duplicate',
                `grate ${gi} is on segment ${grate.segment} side ${SIDE_NAMES[grate.side]}, which ` +
                `already holds grate ${taken.get(here)}`, where));
            continue;
        }
        taken.set(here, gi);
        const other = level.segments[sd.neighbour];
        for (let os = 0; os < SIDE_COUNT; ++os) {
            if (other.sides[os].neighbour === grate.segment) taken.set(`${sd.neighbour}/${os}`, gi);
        }
    }
}

// A pickup or object that is not inside any segment is fatal; a light that is
// not is a warning, because a light outside the mine lights nothing but the
// level still loads.
function checkEntities(level, out) {
    const place = (kind, rule, severity, label, entity) => {
        if (entity.segment >= 0 && entity.segment < level.segments.length) return;
        const [x, y, z] = entity.position;
        out.push(finding(severity, rule,
            `${kind} ${label} at ${x.toFixed(1)} ${y.toFixed(1)} ${z.toFixed(1)} is not inside ` +
            'any segment', {}));
    };

    for (const p of level.pickups) place('pickup', 'pickup-outside', 'error', `'${PICKUP_NAMES[p.kind]}'`, p);
    for (const o of level.objects) place('object', 'object-outside', 'error', `'${o.type}'`, o);
    for (const l of level.lights) place('light', 'light-outside', 'warning', '', l);

    // Both prop kinds are fatal in loadLevel when they are not inside a
    // segment, unlike a static light, so both are errors here.
    for (const b of level.floatingLights) place('floating light', 'ball-outside', 'error', '', b);
    for (const l of level.swingingLights) place('swinging light', 'lamp-outside', 'error', '', l);

    checkCords(level, out);
}

// A lamp hangs a cord's length below its anchor, so a cord longer than the room
// puts the lamp through the floor, where it lights the segment below through
// solid rock.
//
// loadLevel checks the straight-down case and is fatal on it, which is matched
// here. It does not check the swing, so a lamp that only leaves the room at the
// ends of its arc loads and then leaks light - that one is the editor's to
// catch, as a warning, and is in EDITOR_ONLY_RULES.
function checkCords(level, out) {
    for (const lamp of level.swingingLights) {
        if (lamp.segment < 0 || lamp.segment >= level.segments.length) continue;
        const [x, y, z] = lamp.position;
        const where = `at ${x.toFixed(1)} ${y.toFixed(1)} ${z.toFixed(1)}`;

        const straightDown = [lamp.position[0], lamp.position[1] - lamp.cordLength, lamp.position[2]];
        if (findSegment(level, straightDown, lamp.segment) < 0) {
            out.push(finding('error', 'lamp-cord-outside',
                `swinging light ${where} has a ${lamp.cordLength.toFixed(1)}u cord that reaches ` +
                'outside the mine', {}));
            continue;
        }

        for (const angle of [-lamp.amplitude, lamp.amplitude]) {
            if (findSegment(level, lampPosition(lamp, angle), lamp.segment) >= 0) continue;
            out.push(finding('warning', 'lamp-swing-outside',
                `swinging light ${where} hangs clear but swings outside the mine at ` +
                `${Math.abs(angle).toFixed(2)} rad. The game only checks the cord straight down, ` +
                'so this loads and then lights through rock', {}));
            break;
        }
    }
}

// --- the closed-surface check ----------------------------------------------

// The same weld and adjacency as checkClosure() in src/render/shadow.cpp,
// including its 1e-4 quantisation: a report using a different rule could pass
// an asset the loader will then abort on.
const WELD_EPSILON = 1e-4;
const weldKey = (p) => `${Math.round(p[0] / WELD_EPSILON)},${Math.round(p[1] / WELD_EPSILON)},` +
                       `${Math.round(p[2] / WELD_EPSILON)}`;

export function checkClosure(positions, triangles) {
    const unique = new Map();
    const remap = positions.map((p) => {
        const key = weldKey(p);
        if (!unique.has(key)) unique.set(key, unique.size);
        return unique.get(key);
    });

    const edges = new Map();
    let triangleCount = 0;
    for (const tri of triangles) {
        const w = [remap[tri[0]], remap[tri[1]], remap[tri[2]]];
        if (w[0] === w[1] || w[1] === w[2] || w[0] === w[2]) continue;
        ++triangleCount;
        for (let e = 0; e < 3; ++e) {
            const u = w[e];
            const v = w[(e + 1) % 3];
            const key = `${Math.min(u, v)},${Math.max(u, v)}`;
            edges.set(key, (edges.get(key) ?? 0) + 1);
        }
    }

    let openEdges = 0;
    let overSharedEdges = 0;
    for (const uses of edges.values()) {
        if (uses < 2) ++openEdges;
        else if (uses > 2) ++overSharedEdges;
    }

    return {
        weldedVertices: unique.size,
        triangles: triangleCount,
        edges: edges.size,
        openEdges,
        overSharedEdges,
        closed: openEdges === 0 && overSharedEdges === 0,
    };
}

function checkWallSurface(level, out) {
    const report = checkClosure(level.vertices, level.walls.map((w) => w.vi));
    if (!report.closed) {
        out.push(finding('error', 'surface-open',
            `wall surface is not closed - ${report.openEdges} open edge(s), ` +
            `${report.overSharedEdges} over-shared. The mine's own shadow volume is built from ` +
            'it, and an open volume makes depth-fail counting wrong.'));
    }
    return report;
}

// --- checks the game does not make -----------------------------------------

// Nothing in the C++ checks that two segments occupy disjoint space. When they
// overlap, findSegment returns whichever it happens to test first and the ship
// appears to teleport between rooms.
function checkOverlaps(level, out) {
    const n = level.segments.length;
    const boxes = level.segments.map((seg) => {
        const lo = [Infinity, Infinity, Infinity];
        const hi = [-Infinity, -Infinity, -Infinity];
        for (const v of seg.verts) {
            const p = level.vertices[v];
            for (let k = 0; k < 3; ++k) {
                lo[k] = Math.min(lo[k], p[k]);
                hi[k] = Math.max(hi[k], p[k]);
            }
        }
        return { lo, hi };
    });

    for (let a = 0; a < n; ++a) {
        for (let b = a + 1; b < n; ++b) {
            // Broad phase, with a margin: segments that merely touch at a
            // shared portal face are the normal case, not an overlap.
            const A = boxes[a];
            const B = boxes[b];
            let separated = false;
            for (let k = 0; k < 3; ++k) {
                if (A.hi[k] - 0.01 <= B.lo[k] || B.hi[k] - 0.01 <= A.lo[k]) separated = true;
            }
            if (separated) continue;

            // Narrow phase: a centre inside the other segment's six planes is
            // unambiguous. It misses partial overlaps, which is the trade for
            // never crying wolf about two rooms that share a doorway.
            if (insideSegment(level, b, level.segments[a].center, -0.05) ||
                insideSegment(level, a, level.segments[b].center, -0.05)) {
                out.push(finding('warning', 'segment-overlap',
                    `segments ${a} and ${b} occupy the same space. findSegment returns whichever ` +
                    'it tests first, so a ship flying here jumps between them.', { segment: a }));
            }
        }
    }
}

// A segment no portal walk from the player start can reach is either dead data
// or a room nobody will ever see. The game loads it and says nothing.
function checkReachability(level, out) {
    const n = level.segments.length;
    if (level.start.segment < 0 || level.start.segment >= n) return;

    const seen = new Set([level.start.segment]);
    const queue = [level.start.segment];
    for (let head = 0; head < queue.length; ++head) {
        for (const sd of level.segments[queue[head]].sides) {
            if (sd.neighbour < 0 || seen.has(sd.neighbour)) continue;
            seen.add(sd.neighbour);
            queue.push(sd.neighbour);
        }
    }

    for (let si = 0; si < n; ++si) {
        if (!seen.has(si)) {
            out.push(finding('warning', 'segment-unreachable',
                `segment ${si} cannot be reached from the player start by any portal walk`,
                { segment: si }));
        }
    }
}

// A keyed door whose key is never placed opens only if the level script hands
// the key over with give_key(). A level with no script cannot, so there the
// door is shut for good. With a script it may well be deliberate, and the
// editor cannot read Lua to tell, so it says nothing.
function checkDoorKeys(level, out) {
    if (level.script) return;
    const keys = availableKeys(level);
    for (let di = 0; di < level.doors.length; ++di) {
        const door = level.doors[di];
        if (door.key < 0 || keys.has(door.key)) continue;
        out.push(finding('warning', 'door-key-unplaced',
            `door ${di} needs the ${KEY_NAMES[door.key]} key, which is not placed in this level, ` +
            'and the level has no script to give it', { segment: door.segment, side: door.side }));
    }
}

// --- entry point -----------------------------------------------------------

// Returns { findings, closure, ok }. `ok` is false when anything the game
// treats as fatal is present, which is the same thing as "loadLevel would
// abort on this file".
export function validateLevel(level) {
    const findings = [];
    checkStructure(level, findings);

    // The remaining checks index into segments and sides that the structural
    // pass may have just reported as missing; running them anyway turns one
    // real fault into a page of noise.
    const structurallySound = findings.every((f) => f.severity !== 'error');
    let closure = null;
    if (structurallySound) {
        checkDoors(level, findings);
        checkGrates(level, findings);
        checkEntities(level, findings);
        closure = checkWallSurface(level, findings);
        checkOverlaps(level, findings);
        checkReachability(level, findings);
        checkDoorKeys(level, findings);
    }

    return { findings, closure, ok: findings.every((f) => f.severity !== 'error') };
}

export const EDITOR_ONLY_RULES = new Set(['segment-overlap', 'segment-unreachable',
                                          'lamp-swing-outside', 'door-key-unplaced']);
