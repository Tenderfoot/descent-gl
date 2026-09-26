// Placing and editing the things inside a mine: lights, robots and scenery,
// pickups, doors, grates, the exit and the player start.
//
// The rule every one of these has to keep is that an entity's cached `segment`
// says which segment actually contains it. The loader recomputes it for any
// entry that is missing or out of range, but it trusts one that is merely
// WRONG - so an entity dragged across a portal without re-resolving is a light
// that lights the wrong room, or a pickup the player cannot reach. Everything
// here that moves something re-resolves it.

import {
    BALL_COLOR, BLOCK_TYPE, DEFAULTS, KEY_NAMES, LAMP_COLOR, MAX_FLOATING_LIGHTS, MAX_SWINGING_LIGHTS,
    PICKUP_NAMES, SIDE_COUNT, SIDE_NAMES, SIDE_TOP, SIDE_BOTTOM, UV_MODES,
} from './format.js';
import { findSegment, sideCorners } from './geometry.js';
import { fromLevelQuat, multiplyQuat, toLevelQuat, transformDirection } from './transform.js';

// The lists an entity kind lives in, so the UI can be written once.
export const ENTITY_LISTS = {
    light: 'lights',
    pickup: 'pickups',
    object: 'objects',
    // The two kinds that move once the level is running. A swinging light's
    // `position` is its ANCHOR - the point the cord hangs from - which is what
    // the author places and drags; the lamp itself is derived from it. See
    // parseLevel for why it is not called `anchor` in here.
    ball: 'floatingLights',
    lamp: 'swingingLights',
};

export function entityList(level, kind) {
    const name = ENTITY_LISTS[kind];
    if (!name) throw new Error(`no such entity kind: ${kind}`);
    return level[name];
}

// Where a new thing goes when the author has not aimed: the centre of the
// segment they are looking at.
export function segmentCentre(level, si) {
    return level.segments[si].center.slice();
}

function place(level, entity) {
    entity.segment = findSegment(level, entity.position, entity.segment);
    return entity;
}

// --- adding -----------------------------------------------------------------

export function addLight(level, position, segment = -1) {
    const light = place(level, {
        keyOrder: null,
        position: position.slice(),
        radius: DEFAULTS.lightRadius,
        color: [1, 1, 1],
        intensity: DEFAULTS.lightIntensity,
        type: 'point',
        direction: [0, -1, 0],
        coneDegrees: DEFAULTS.lightCone,
        fixtureSize: DEFAULTS.lightFixtureSize,
        segment,
    });
    level.lights.push(light);
    return level.lights.length - 1;
}

export function addPickup(level, kind, position) {
    if (kind < 0 || kind >= PICKUP_NAMES.length) throw new Error(`no such pickup kind: ${kind}`);
    level.pickups.push(place(level, { keyOrder: null, kind, position: position.slice(), segment: -1 }));
    return level.pickups.length - 1;
}

export function addObject(level, type, position) {
    if (!type) throw new Error('an object needs a model name');
    const object = {
        keyOrder: null,
        type,
        position: position.slice(),
        orientation: [1, 0, 0, 0],
        scale: DEFAULTS.objectScale,
        segment: -1,
        health: DEFAULTS.objectHealth,
    };
    if (type === BLOCK_TYPE) {
        object.size = DEFAULTS.blockSize.slice();
        object.texture = DEFAULTS.blockTexture;
        object.textureMode = DEFAULTS.blockTextureMode;
    }
    level.objects.push(place(level, object));
    return level.objects.length - 1;
}

// Where a new lamp goes when the author has not aimed: hung from the middle of
// the segment's ceiling, on a cord short enough to hang clear of its floor.
//
// A lamp dropped at the segment centre on the default cord would often be
// fine and sometimes be through the floor, and the game is fatal on the
// second case. Deriving both from the room means placing one always works.
export function ceilingAnchor(level, si) {
    const centre = level.segments[si].center;
    const top = sideCorners(level, si, SIDE_TOP);
    const bottom = sideCorners(level, si, SIDE_BOTTOM);
    const mid = (corners) => [0, 1, 2].map(
        (k) => (corners[0][k] + corners[1][k] + corners[2][k] + corners[3][k]) / 4);

    const ceiling = mid(top);
    const floor = mid(bottom);
    // Just under the ceiling rather than in it: an anchor exactly on a face is
    // the case findSegment is entitled to answer -1 for.
    const position = [ceiling[0], ceiling[1] - 0.4, ceiling[2]];
    const drop = Math.max(1.0, position[1] - floor[1]);
    return { position: findSegment(level, position, si) >= 0 ? position : centre.slice(),
             cordLength: Math.min(DEFAULTS.lampCordLength, drop * 0.6) };
}

// Both prop kinds are capped in the simulation: past the cap the game warns and
// ignores the rest, which is a level that silently loses the thing you just
// placed. Refusing here means finding out while placing it.
export function addFloatingLight(level, position) {
    if (level.floatingLights.length >= MAX_FLOATING_LIGHTS) {
        throw new Error(`a level may have at most ${MAX_FLOATING_LIGHTS} floating lights`);
    }
    level.floatingLights.push(place(level, {
        keyOrder: null,
        position: position.slice(),
        radius: DEFAULTS.ballRadius,
        color: BALL_COLOR.slice(),
        lightRadius: DEFAULTS.ballLightRadius,
        lightIntensity: DEFAULTS.ballLightIntensity,
        restitution: DEFAULTS.ballRestitution,
        mass: DEFAULTS.ballMass,
        drift: DEFAULTS.ballDrift,
        segment: -1,
    }));
    return level.floatingLights.length - 1;
}

// `position` is the anchor. A new lamp gets a cord short enough to hang clear
// of whatever is below the point it was dropped at, because the game is fatal
// on a cord that reaches outside the mine and the author would otherwise find
// that out from a failed launch.
export function addSwingingLight(level, position, cordLength = DEFAULTS.lampCordLength) {
    if (level.swingingLights.length >= MAX_SWINGING_LIGHTS) {
        throw new Error(`a level may have at most ${MAX_SWINGING_LIGHTS} swinging lights`);
    }
    level.swingingLights.push(place(level, {
        keyOrder: null,
        position: position.slice(),
        cordLength,
        amplitude: DEFAULTS.lampAmplitude,
        rate: DEFAULTS.lampRate,
        phase: DEFAULTS.lampPhase,
        swingAxis: [1, 0, 0],
        color: LAMP_COLOR.slice(),
        lightRadius: DEFAULTS.lampLightRadius,
        lightIntensity: DEFAULTS.lampLightIntensity,
        coneDegrees: DEFAULTS.lampCone,
        segment: -1,
    }));
    return level.swingingLights.length - 1;
}

// Where a lamp's housing sits at a given swing angle. The same construction as
// sim::lampPosition in src/sim/props.cpp: straight down from the anchor, tilted
// by `angle` along the swing axis, with the cord taut. The editor draws the
// swing from this, and validate.js checks the extremes with it.
export function lampPosition(lamp, angle) {
    const axis = normalise(lamp.swingAxis, [1, 0, 0]);
    const c = Math.cos(angle);
    const s = Math.sin(angle);
    return [
        lamp.position[0] + axis[0] * s * lamp.cordLength,
        lamp.position[1] - c * lamp.cordLength + axis[1] * s * lamp.cordLength,
        lamp.position[2] + axis[2] * s * lamp.cordLength,
    ];
}

function normalise(v, fallback) {
    const length = Math.hypot(v[0], v[1], v[2]);
    return length > 1e-5 ? [v[0] / length, v[1] / length, v[2] / length] : fallback.slice();
}

// A door fills a portal. The game is fatal on a door anywhere else - there
// would be nothing for it to open into - so this refuses rather than letting
// the author find out from the validator.
//
// `hatch` is the second, independent lock: an emergency hatch stays shut
// until the reactor is destroyed, whatever keys the player is carrying. A
// door may carry both, in which case it wants the key and the reactor.
export function addDoor(level, segment, side, key = -1, hatch = false) {
    const sd = level.segments[segment].sides[side];
    if (sd.neighbour < 0) {
        throw new Error(`segment ${segment} ${SIDE_NAMES[side]} is a solid wall. A door has to be ` +
                        'a portal - there must be somewhere for it to open into.');
    }
    if (sd.door >= 0) throw new Error('that portal already has a door');
    if (sd.grate >= 0) throw new Error('that portal already has a grate');
    if (key < -1 || key >= KEY_NAMES.length) throw new Error(`no such key colour: ${key}`);

    level.doors.push({
        keyOrder: null,
        segment,
        side,
        key,
        hatch,
        openDistance: DEFAULTS.doorOpenDistance,
        openTime: DEFAULTS.doorOpenTime,
        closeDelay: DEFAULTS.doorCloseDelay,
    });
    rewireDoors(level);
    return level.doors.length - 1;
}

// A grate fills a portal with a cut-out panel: seen, lit and shot through,
// not flown through. The game is fatal on one anywhere but an empty portal -
// a wall has nothing behind it to see, and a door and a grate cannot share an
// opening - so this refuses the same things.
//
// `texture` is a tile layer, and it is the tile's alpha that makes the holes.
// A tile with none is a grate you cannot see through, which is legal and
// almost certainly not what was meant; the side panel offers every tile and
// leaves that to the author.
export function addGrate(level, segment, side, texture) {
    const sd = level.segments[segment].sides[side];
    if (sd.neighbour < 0) {
        throw new Error(`segment ${segment} ${SIDE_NAMES[side]} is a solid wall. A grate fills ` +
                        'a portal - there must be something on the other side of it.');
    }
    if (sd.door >= 0) throw new Error('that portal already has a door');
    if (sd.grate >= 0) throw new Error('that portal already has a grate');
    if (!Number.isInteger(texture) || texture < 0) throw new Error(`no such tile: ${texture}`);

    level.grates.push({ keyOrder: null, segment, side, texture, uvMode: null });
    rewireGrates(level);
    return level.grates.length - 1;
}

export function setGrateTexture(level, index, texture) {
    const grate = level.grates[index];
    if (!grate) throw new Error(`no grate ${index}`);
    if (!Number.isInteger(texture) || texture < 0) throw new Error(`no such tile: ${texture}`);
    grate.texture = texture;
}

// 'side', 'world', or null to take the level's. The editor draws grates flat,
// so this changes the file and the game, not the view.
export function setGrateUvMode(level, index, mode) {
    const grate = level.grates[index];
    if (!grate) throw new Error(`no grate ${index}`);
    if (mode !== null && !UV_MODES.includes(mode)) throw new Error(`no such uv mode: ${mode}`);
    grate.uvMode = mode;
}

export function setExit(level, position, radius = DEFAULTS.exitRadius) {
    level.exitTrigger = place(level, {
        keyOrder: level.exitTrigger?.keyOrder ?? null,
        position: position.slice(),
        radius,
        segment: -1,
    });
}

// --- duplicating ------------------------------------------------------------

// The caps the two prop kinds are under, so a clone cannot walk past a limit
// the add path refuses to cross.
const CAPS = {
    ball: { limit: MAX_FLOATING_LIGHTS, what: 'floating lights' },
    lamp: { limit: MAX_SWINGING_LIGHTS, what: 'swinging lights' },
};

// A second copy of an entity, standing exactly where the first one does.
//
// Placing several of a thing in one room otherwise means adding one at the
// segment's centre and dragging it back, and - the part that actually costs
// the time - tuning its seven or eight fields again to match the one beside
// it. A clone arrives already matching, and the caller selects it, so the
// gizmo is on the copy and the first drag moves it off its source.
//
// Exactly on top rather than offset by some amount: the source is inside a
// segment, so the copy is too, which is the one placement that can never
// produce the out-of-segment entity the game is fatal on. It does mean the two
// are indistinguishable until the copy is dragged away, which is why this is
// only ever reached from a path that selects the copy.
export function duplicateEntity(level, kind, index) {
    // There is one player start and one exit, so a second copy of either would
    // silently replace the original rather than join it; a door belongs to a
    // portal rather than to a place, and a portal takes one.
    if (!ENTITY_LISTS[kind]) throw new Error(`a ${kind} cannot be duplicated`);

    const list = entityList(level, kind);
    const source = list[index];
    if (!source) throw new Error(`no ${kind} ${index}`);

    const cap = CAPS[kind];
    if (cap && list.length >= cap.limit) {
        throw new Error(`a level may have at most ${cap.limit} ${cap.what}`);
    }

    // Deep, because an entity's arrays - a position, a colour, a direction or
    // an orientation - would otherwise be shared with the source, and a copy
    // that moves when the thing it was copied from moves is not a copy. Copying
    // every own field rather than a named list means a field added to one of
    // these kinds later is cloned without anyone having to remember it here.
    const copy = {};
    for (const [key, value] of Object.entries(source)) copy[key] = cloneValue(value);

    // The position is unchanged, so the cached segment is right by
    // construction - but this file's rule is that the cache is re-resolved
    // rather than trusted, and place() costs one lookup.
    list.push(place(level, copy));
    return list.length - 1;
}

function cloneValue(value) {
    return Array.isArray(value) ? value.map(cloneValue) : value;
}

// --- moving and removing ----------------------------------------------------

export function moveEntity(level, kind, index, position) {
    if (kind === 'start') {
        level.start.position = position.slice();
        level.start.segment = findSegment(level, position, level.start.segment);
        return;
    }
    if (kind === 'exit') {
        if (!level.exitTrigger) throw new Error('this level has no exit');
        level.exitTrigger.position = position.slice();
        level.exitTrigger.segment = findSegment(level, position, level.exitTrigger.segment);
        return;
    }
    const entity = entityList(level, kind)[index];
    if (!entity) throw new Error(`no ${kind} ${index}`);
    entity.position = position.slice();
    entity.segment = findSegment(level, position, entity.segment);
}

// Whatever facing a kind carries, and where it lives.
//
// A kind that is not in here has none: a pickup, a floating ball and the exit
// trigger are spheres as far as the level is concerned, and a group rotation
// only carries them around the pivot.
const FACING = {
    light: { field: 'direction', quaternion: false },
    lamp: { field: 'swingAxis', quaternion: false },
    object: { field: 'orientation', quaternion: true },
};

// Turns an entity's own facing by a group rotation.
//
// Separate from moveEntity because the two are separate questions - a
// translation moves a thing without turning it - and because most kinds have no
// answer to this one. A cluster of scenery rotated ninety degrees and still
// facing the way it was placed reads as a bug, which is why this exists at all.
//
// `rotation` is a quaternion in [x, y, z, w] order, as transform.js uses.
//
// `base` is the facing to rotate FROM, and a drag has to supply it. The
// rotation a gizmo reports is absolute - the whole turn so far, not this
// frame's part of it - so composing it onto the entity's current facing once
// per frame would apply the same turn sixty times a second and spin the thing
// off into the rock. Given a base, this is idempotent: calling it repeatedly
// with the same rotation lands in the same place every time.
export function turnEntity(level, kind, index, rotation, base = null) {
    if (kind === 'start') {
        level.start.orientation =
            toLevelQuat(multiplyQuat(rotation, fromLevelQuat(base ?? level.start.orientation)));
        return;
    }

    const facing = FACING[kind];
    if (!facing) return;
    const entity = entityList(level, kind)[index];
    if (!entity) throw new Error(`no ${kind} ${index}`);

    // A point light has a `direction` field that nothing reads. Turning it
    // would put a direction in the file for a light that has no cone, and the
    // writer omits defaulted fields precisely so that the file says only what
    // is true of the level.
    if (kind === 'light' && entity.type !== 'spot') return;

    const from = base ?? entity[facing.field];
    if (facing.quaternion) {
        entity[facing.field] = toLevelQuat(multiplyQuat(rotation, fromLevelQuat(from)));
        return;
    }
    entity[facing.field] = transformDirection({ rotation }, from);
}

// The facing an entity currently carries, or null for a kind that has none.
// Recorded at the start of a drag and handed back to turnEntity as its `base`.
export function entityFacing(level, kind, index) {
    if (kind === 'start') return level.start.orientation.slice();
    const facing = FACING[kind];
    if (!facing) return null;
    if (kind === 'light' && entityList(level, kind)[index]?.type !== 'spot') return null;
    const value = entityList(level, kind)[index]?.[facing.field];
    return value ? value.slice() : null;
}

// Several entities at once, from a group transform.
//
// Each move goes through moveEntity, so every entity's cached `segment` is
// re-resolved one by one. That is deliberately not optimised into a single pass:
// findSegment takes the previous segment as a hint, and a group being dragged
// across a portal is exactly the case where the hint is right for some members
// and wrong for others.
export function moveEntities(level, moves) {
    for (const { kind, index, position, rotation, facing } of moves) {
        moveEntity(level, kind, index, position);
        if (rotation) turnEntity(level, kind, index, rotation, facing ?? null);
    }
}

export function removeEntity(level, kind, index) {
    if (kind === 'exit') {
        level.exitTrigger = null;
        return;
    }
    if (kind === 'door') {
        level.doors.splice(index, 1);
        rewireDoors(level);
        return;
    }
    if (kind === 'grate') {
        level.grates.splice(index, 1);
        rewireGrates(level);
        return;
    }
    if (kind === 'start') throw new Error('a level needs a player start');
    const list = entityList(level, kind);
    if (!list[index]) throw new Error(`no ${kind} ${index}`);
    list.splice(index, 1);
}

export function setStartOrientation(level, orientation) {
    level.start.orientation = orientation.slice();
}

// --- doors ------------------------------------------------------------------

// Rebuilds every side's `door` index from level.doors, on both sides of each
// portal. loadLevel does this at load and the game reads it from both ends, so
// "is the way out of here shut?" costs no search. Anything that adds, removes
// or reorders a door has to call this.
export function rewireDoors(level) {
    for (const seg of level.segments) for (const sd of seg.sides) sd.door = -1;

    for (let di = 0; di < level.doors.length; ++di) {
        const door = level.doors[di];
        const seg = level.segments[door.segment];
        const sd = seg?.sides[door.side];
        if (!sd || sd.neighbour < 0) continue;

        sd.door = di;
        const other = level.segments[sd.neighbour];
        for (let os = 0; os < SIDE_COUNT; ++os) {
            if (other.sides[os].neighbour === door.segment) other.sides[os].door = di;
        }
    }
}

// The same for `grate`, from level.grates. loadLevel wires both ends, and the
// side panel asks a side which grate it holds from either of them.
export function rewireGrates(level) {
    for (const seg of level.segments) for (const sd of seg.sides) sd.grate = -1;

    for (let gi = 0; gi < level.grates.length; ++gi) {
        const grate = level.grates[gi];
        const sd = level.segments[grate.segment]?.sides[grate.side];
        if (!sd || sd.neighbour < 0) continue;

        sd.grate = gi;
        const other = level.segments[sd.neighbour];
        for (let os = 0; os < SIDE_COUNT; ++os) {
            if (other.sides[os].neighbour === grate.segment) other.sides[os].grate = gi;
        }
    }
}

// --- validation helpers the UI uses ----------------------------------------

// Is there a reactor to destroy? An emergency hatch in a level without one
// can never open, which is the same trap as a key that is never placed, so
// the door editor asks this before offering a hatch.
//
// By robot type name, because that is all a level file says: the reactor is
// an object whose type the robot config marks isReactor, and 'reactor' is
// what that type is called.
export function hasReactor(level) {
    return level.objects.some((object) => object.type === 'reactor');
}

// Which key colours are collectable in this level, so the door editor cannot
// offer a key that does not exist and gate the mine shut forever.
export function availableKeys(level) {
    const found = new Set();
    for (const pickup of level.pickups) {
        const name = PICKUP_NAMES[pickup.kind];
        const index = KEY_NAMES.indexOf(name.replace('key_', ''));
        if (name.startsWith('key_') && index >= 0) found.add(index);
    }
    return found;
}
