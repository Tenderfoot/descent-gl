// Making a level that does not exist yet.
//
// The smallest thing the game will load: one segment, eight vertices, six
// walls, a player start inside it, and one light so the room is not simply
// black. Everything else is grown from it with extrude.
//
// This is deliberately not a template with rooms in it. A starter level that
// guesses at a layout is a layout the author has to delete first, and the
// point of the editor is that growing a mine is quick.

import { parseLevel } from './level.js';

// A cube of `size` with its near-bottom-left corner at the origin, in the
// corner order the format uses:
//   0 (-x,-y,-z)  1 (+x,-y,-z)  2 (+x,+y,-z)  3 (-x,+y,-z)
//   4 (-x,-y,+z)  5 (+x,-y,+z)  6 (+x,+y,+z)  7 (-x,+y,+z)
export function cubeVertices(size = 20) {
    return [
        [0, 0, 0], [size, 0, 0], [size, size, 0], [0, size, 0],
        [0, 0, size], [size, 0, size], [size, size, size], [0, size, size],
    ];
}

// Returns a parsed level, so it has gone through exactly the same reader a
// file does and carries the same derived data. Building the object by hand
// here would be a second definition of what a level is.
export function newLevel(name, size = 20) {
    const half = size / 2;
    const json = {
        name,
        comment: 'Created in the level editor. One room; grow the rest with extrude.',
        vertices: cubeVertices(size),
        segments: [{
            verts: [0, 1, 2, 3, 4, 5, 6, 7],
            // Rock on five sides, panel on the floor, so the starter room is
            // not one flat colour and the texture indices are visibly a thing
            // the author can change.
            sides: [{ texture: 0 }, { texture: 0 }, { texture: 0 },
                    { texture: 1 }, { texture: 0 }, { texture: 0 }],
        }],
        lights: [{
            position: [half, size * 0.8, half],
            radius: size * 2,
            color: [1, 0.92, 0.8],
            intensity: 2.0,
            type: 'point',
            segment: 0,
        }],
        start: {
            segment: 0,
            position: [half, half, half],
            // Identity: looking down -Z, which is the direction the ship faces.
            orientation: [1, 0, 0, 0],
        },
    };

    return parseLevel(json, `assets/levels/${name}.json`);
}

// The server accepts any bare stem; this is the same rule, so the editor can
// say no before a round trip rather than after.
export const NAME_PATTERN = /^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$/;

export function checkName(name, existing) {
    if (!NAME_PATTERN.test(name ?? '')) {
        return 'a level name is letters, digits, dash and underscore, starting with a letter or digit';
    }
    if (existing.includes(name)) return `${name} already exists — open it instead`;
    return null;
}
