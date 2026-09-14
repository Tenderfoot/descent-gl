// The level format, mirrored from src/game/level.h.
//
// Everything in this file is a transcription of a C++ declaration, and the
// only reason it may be edited is that the C++ changed. The tables are the
// load-bearing ones: get kSideVertices wrong and the editor draws a mine that
// is subtly not the mine the game builds, with no error anywhere.

// Side order is fixed. The renderer, the validator and the level files all
// depend on it. src/game/level.h: enum Side.
export const SIDE_LEFT = 0;
export const SIDE_TOP = 1;
export const SIDE_RIGHT = 2;
export const SIDE_BOTTOM = 3;
export const SIDE_BACK = 4;
export const SIDE_FRONT = 5;
export const SIDE_COUNT = 6;

export const SIDE_NAMES = ['left', 'top', 'right', 'bottom', 'back', 'front'];

// Which of the segment's 8 vertices make up each side, wound counter-clockwise
// seen from INSIDE the segment. The mine is a space you are inside, so a
// wall's front face and its normal both point inward.
//
// Cube corner numbering, before any deformation:
//   0 (-x,-y,-z)  1 (+x,-y,-z)  2 (+x,+y,-z)  3 (-x,+y,-z)
//   4 (-x,-y,+z)  5 (+x,-y,+z)  6 (+x,+y,+z)  7 (-x,+y,+z)
export const SIDE_VERTICES = [
    [0, 3, 7, 4],   // left   (-x)
    [3, 2, 6, 7],   // top    (+y)
    [1, 5, 6, 2],   // right  (+x)
    [0, 4, 5, 1],   // bottom (-y)
    [0, 1, 2, 3],   // back   (-z)
    [4, 7, 6, 5],   // front  (+z)
];

export const OPPOSITE_SIDE = [SIDE_RIGHT, SIDE_BOTTOM, SIDE_LEFT, SIDE_TOP, SIDE_FRONT, SIDE_BACK];

export const KEY_NAMES = ['blue', 'yellow', 'red'];

export const PICKUP_NAMES = [
    'shield', 'energy', 'laser', 'concussion', 'homing', 'flares',
    'key_blue', 'key_yellow', 'key_red',
];

// src/render/procedural.h: enum WallTile. A level file's `texture: 2` means
// the same thing here as it does in the game.
export const WALL_TILE_NAMES = ['rock', 'panel', 'hazard', 'grate', 'hull'];

// src/game/level.cpp: kWorldUnitsPerTile. One texture tile per this many world
// units, for sides with no authored UVs.
export const WORLD_UNITS_PER_TILE = 5.5;

// Defaults, so a round trip does not write a field the C++ would have omitted.
// src/game/level.cpp: loadLevel / saveLevel.
export const DEFAULTS = {
    lightRadius: 30.0,
    lightIntensity: 1.0,
    lightCone: 45.0,
    doorOpenDistance: 11.0,
    doorOpenTime: 0.9,
    doorCloseDelay: 2.5,
    exitRadius: 6.0,
    objectScale: 1.0,
    objectHealth: 0.0,
};

// Corner order for each of the two candidate triangulations of a quad.
export const SPLIT_A = [[0, 1, 2], [0, 2, 3]];   // diagonal 0-2
export const SPLIT_B = [[0, 1, 3], [1, 2, 3]];   // diagonal 1-3
