// Placing and editing the things inside a mine: lights, robots and scenery,
// pickups, doors, the exit and the player start.
//
// The rule every one of these has to keep is that an entity's cached `segment`
// says which segment actually contains it. The loader recomputes it for any
// entry that is missing or out of range, but it trusts one that is merely
// WRONG - so an entity dragged across a portal without re-resolving is a light
// that lights the wrong room, or a pickup the player cannot reach. Everything
// here that moves something re-resolves it.

import { DEFAULTS, KEY_NAMES, PICKUP_NAMES, SIDE_COUNT, SIDE_NAMES } from './format.js';
import { findSegment } from './geometry.js';

// The lists an entity kind lives in, so the UI can be written once.
export const ENTITY_LISTS = {
    light: 'lights',
    pickup: 'pickups',
    object: 'objects',
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
    level.objects.push(place(level, {
        keyOrder: null,
        type,
        position: position.slice(),
        orientation: [1, 0, 0, 0],
        scale: DEFAULTS.objectScale,
        segment: -1,
        health: DEFAULTS.objectHealth,
    }));
    return level.objects.length - 1;
}

// A door fills a portal. The game is fatal on a door anywhere else - there
// would be nothing for it to open into - so this refuses rather than letting
// the author find out from the validator.
export function addDoor(level, segment, side, key = -1) {
    const sd = level.segments[segment].sides[side];
    if (sd.neighbour < 0) {
        throw new Error(`segment ${segment} ${SIDE_NAMES[side]} is a solid wall. A door has to be ` +
                        'a portal - there must be somewhere for it to open into.');
    }
    if (sd.door >= 0) throw new Error('that portal already has a door');
    if (key < -1 || key >= KEY_NAMES.length) throw new Error(`no such key colour: ${key}`);

    level.doors.push({
        keyOrder: null,
        segment,
        side,
        key,
        openDistance: DEFAULTS.doorOpenDistance,
        openTime: DEFAULTS.doorOpenTime,
        closeDelay: DEFAULTS.doorCloseDelay,
    });
    rewireDoors(level);
    return level.doors.length - 1;
}

export function setExit(level, position, radius = DEFAULTS.exitRadius) {
    level.exitTrigger = place(level, {
        keyOrder: level.exitTrigger?.keyOrder ?? null,
        position: position.slice(),
        radius,
        segment: -1,
    });
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

// --- validation helpers the UI uses ----------------------------------------

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
