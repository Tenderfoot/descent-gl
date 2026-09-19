// Growing a whole mine at once.
//
// The editor's New button makes one room and leaves the rest to the author.
// This makes a mine: rooms, the loops between them, lights, robots, a key
// chain, a reactor and the way out. It exists because the shipped content is
// one complete level and seven fragments, and "another level to fly" should
// not cost an evening of extruding.
//
// It drives `extrudeSide` and `joinSides` rather than writing segments into an
// array. That is the whole reason this file is short: those two operations
// already make portals exact by construction - shared vertex INDICES, not
// vertices that happen to coincide - and reimplementing them here would be a
// second definition of what a level is, which is the mistake `create.js`
// declines to make for one room and this declines to make for forty.
//
// Free of any three.js import, so tools/editor/tests can run it under node.

import { addDoor, addLight, addObject, addPickup, segmentCentre, setExit } from './entities.js';
import { newLevel } from './create.js';
import { SIDE_VERTICES } from './format.js';
import { add, insideSegment, scale } from './geometry.js';
import { extrudeSide, joinSides, moveVertices, setTexture } from './ops.js';
import { validateLevel } from './validate.js';

// The lattice pitch. Every room is grown as a cube of this size and every
// extrusion is exactly this deep, which is what makes a loop closeable: two
// rooms that are neighbours on the lattice have faces in exactly the same
// place, so `joinSides` welds them without moving anything. Deformation comes
// afterwards, once every join is already made out of shared indices and can no
// longer come apart.
// The lattice pitch, and it is deliberately NOT a cube.
//
// A room that is a perfect cube has its centre exactly on the diagonal through
// four of its edges and both of its corner diagonals. The game fires its
// collision rays from segment centres along all 26 face, edge and corner
// directions, and `raycast` uses Moller-Trumbore with no edge slack - on
// purpose, so a wall never over-reports and stops a shot short. Those two
// facts together mean a ray down a cube's corner diagonal can miss every
// triangle meeting at that corner and report a ray leaving a closed mine.
//
// Neither hand-built level trips it, because neither contains a perfect cube.
// A generator that emitted nothing else tripped it on the first ray every
// time. Three pitches that share no simple ratio cost nothing, keep every
// built room an exact axis-aligned box, and move the centre off all thirteen
// of those axes.
const CELL = [20, 17, 23];
const ROOM = CELL[0];

// Which pitch a side extrudes along. left/right x, top/bottom y, back/front z.
const SIDE_PITCH = [CELL[0], CELL[1], CELL[0], CELL[1], CELL[2], CELL[2]];

// Wall tiles, from src/render/procedural.h.
const WALL_ROCK = 0;
const WALL_PANEL = 1;
const WALL_GRATE = 3;

// Pickup kinds, from src/game/level.h: shield, energy, laser, concussion,
// homing, flares, then the three keys.
const PICKUP_SHIELD = 0;
const PICKUP_ENERGY = 1;
const PICKUP_CONCUSSION = 3;
const PICKUP_FLARES = 5;
const PICKUP_KEY = [6, 7, 8];   // blue, yellow, red - the order `KEY_NAMES` uses

// Lattice directions, paired with the side each one leaves through.
// src/game/level.h: left -x, top +y, right +x, bottom -y, back -z, front +z.
const STEPS = [
    { delta: [-1, 0, 0], side: 0 },
    { delta: [0, 1, 0], side: 1 },
    { delta: [1, 0, 0], side: 2 },
    { delta: [0, -1, 0], side: 3 },
    { delta: [0, 0, -1], side: 4 },
    { delta: [0, 0, 1], side: 5 },
];
const OPPOSITE_STEP = [2, 3, 0, 1, 5, 4];

export const GENERATOR_DEFAULTS = {
    rooms: 24,
    loops: 3,
    keys: 1,
    robots: 8,
    // How much of the mine is raw rock rather than built structure. The built
    // stretches stay on the lattice and take the panel and grate tiles; the
    // rock stretches get pushed around. 0 is a station, 1 is a cave.
    rockFraction: 0.6,
    // How far a corner of a rock room may be pushed, as a fraction of the
    // lattice pitch. Past about 0.2 the segments start to fold through
    // themselves and the validator rejects the attempt.
    roughness: 0.13,
    // What fraction of rock rooms get a lamp. Deliberately not all of them:
    // the renderer has no ambient term and the dark is the point, so an unlit
    // stretch is content rather than an omission.
    litRock: 0.45,
    settle: 0,
    seed: 0,
};

// mulberry32. Small, fast, and the same sequence in every JS engine, which is
// what makes a seed worth showing to the author: type it back in and you get
// the same mine.
export function makeRng(seed) {
    let a = (seed >>> 0) || 1;
    return function rng() {
        a = (a + 0x6d2b79f5) | 0;
        let t = Math.imul(a ^ (a >>> 15), 1 | a);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

const pick = (rng, list) => list[Math.floor(rng() * list.length) % list.length];
const key = (cell) => `${cell[0]},${cell[1]},${cell[2]}`;

// --- the lattice -----------------------------------------------------------

// Choose which lattice cells the mine occupies.
//
// The constraint that shapes everything here: EVERY pair of occupied cells
// that touch has to become a portal. Leaving one as a pair of walls does not
// give you two rooms with rock between them - the rooms are face to face with
// nothing in between, `pushVertex` welds their corners, and the wall surface
// ends up with four triangles on an edge that may only have two. The closure
// check calls that "over-shared" and it is right: the mine's own shadow volume
// is built from that surface.
//
// Which turns out to be how the loop count gets to be exact rather than
// approximate. A cell placed where it touches exactly ONE existing cell adds a
// room and an edge, so the graph stays a tree. A cell placed where it touches
// exactly TWO adds a room and two edges, which is one independent cycle. So
// the mine is grown in two phases and the number of ways round it is chosen,
// not discovered.
//
// Vertical steps are rarer than horizontal ones on purpose. A mine that climbs
// as readily as it spreads reads as a lattice; one that mostly spreads and
// occasionally drops reads as dug.
function growCells(rng, count, loops) {
    const occupied = new Map([[key([0, 0, 0]), true]]);
    const cells = [[0, 0, 0]];

    const touching = (cell) =>
        STEPS.reduce((n, step) => n + (occupied.has(key(add(cell, step.delta))) ? 1 : 0), 0);

    // Would putting a room here pinch the wall surface?
    //
    // Two rooms that meet only along an EDGE - diagonal neighbours with
    // nothing between them - are not two rooms with rock between them either.
    // Their four walls all run into that one line, so the surface has four
    // triangles on an edge that may only ever have two. The closure check
    // calls it over-shared, and `deleteSegment` carries the same warning for
    // the same shape: it is a pinch the shadow volumes cannot use, and no
    // amount of cleverness afterwards fixes it because the geometry genuinely
    // pinches.
    //
    // The same is true of a corner-only contact, one lattice vertex with two
    // rooms diagonally opposite across it. So a placement is refused unless
    // every diagonal neighbour it would acquire is reachable through a face
    // that is also occupied.
    const wouldPinch = (cell) => {
        for (let i = 0; i < STEPS.length; ++i) {
            for (let j = 0; j < STEPS.length; ++j) {
                if (i === j || OPPOSITE_STEP[i] === j) continue;
                const a = add(cell, STEPS[i].delta);
                const b = add(cell, STEPS[j].delta);
                const diagonal = add(a, STEPS[j].delta);
                if (!occupied.has(key(diagonal))) continue;
                // Sharing an edge is fine as long as one of the two rooms that
                // fill the corner is there to carry the surface round it.
                if (!occupied.has(key(a)) && !occupied.has(key(b))) return true;
            }
        }
        // And the corner case, across a lattice vertex.
        for (const sx of [-1, 1]) {
            for (const sy of [-1, 1]) {
                for (const sz of [-1, 1]) {
                    const corner = add(cell, [sx, sy, sz]);
                    if (!occupied.has(key(corner))) continue;
                    const faces = [[sx, 0, 0], [0, sy, 0], [0, 0, sz]];
                    if (!faces.some((f) => occupied.has(key(add(cell, f))))) return true;
                }
            }
        }
        return false;
    };

    // Every empty cell next to an occupied one, weighted against climbing.
    // Every empty cell next to an occupied one, weighted two ways.
    //
    // Against climbing, because a mine that goes up as readily as it spreads
    // reads as a lattice and one that mostly spreads reads as dug. And
    // towards the outside, because growing from a random frontier cell balls
    // the mine up into a blob - and in a blob every room can see every other,
    // which `--world-test` reports as "portal culling is doing nothing". It is
    // right to: a mine you can see all of at once is a room, not a mine.
    const candidates = () => {
        const middle = cells.reduce((acc, c) => add(acc, c), [0, 0, 0])
            .map((v) => v / cells.length);
        const seen = new Map();
        for (const cell of cells) {
            for (const step of STEPS) {
                const next = add(cell, step.delta);
                const id = key(next);
                if (occupied.has(id) || seen.has(id)) continue;
                const reach = Math.hypot(next[0] - middle[0], next[1] - middle[1],
                                         next[2] - middle[2]);
                const weight = (step.delta[1] !== 0 ? 1 : 3) * (1 + Math.round(reach));
                seen.set(id, { cell: next, weight });
            }
        }
        return [...seen.values()];
    };

    // A cell that touches two existing rooms adds a way round. Whether that
    // way round is one the game will accept depends on WHAT it goes round.
    //
    // Four rooms in a filled square are a loop you can fly: the union is still
    // a solid block, so the wall surface is still a sphere and its Euler
    // characteristic is still 2. Eight rooms ringing an empty cell are a loop
    // too - and the union is a doughnut, the surface is a torus, and
    // `--world-test` rejects it outright because the shadow volumes are built
    // from that surface. mine01 has eight ways round and passes; both shipped
    // levels that ring something (FigureEight, MineEntrance) do not.
    //
    // So a loop is only taken when the diagonal that completes the square is
    // already occupied. Same graph cycle, no handle.
    const completesSquare = (cell) => {
        for (let i = 0; i < STEPS.length; ++i) {
            for (let j = 0; j < STEPS.length; ++j) {
                if (i === j || OPPOSITE_STEP[i] === j) continue;
                if (!occupied.has(key(add(cell, STEPS[i].delta)))) continue;
                if (!occupied.has(key(add(cell, STEPS[j].delta)))) continue;
                const diagonal = add(add(cell, STEPS[i].delta), STEPS[j].delta);
                if (occupied.has(key(diagonal))) return true;
            }
        }
        return false;
    };

    const place = (wanted) => {
        const options = [];
        for (const { cell, weight } of candidates()) {
            if (touching(cell) !== wanted) continue;
            if (wouldPinch(cell)) continue;
            if (wanted >= 2 && !completesSquare(cell)) continue;
            for (let w = 0; w < weight; ++w) options.push(cell);
        }
        if (options.length === 0) return false;
        const chosen = pick(rng, options);
        occupied.set(key(chosen), true);
        cells.push(chosen);
        return true;
    };

    const treeRooms = Math.max(2, count - Math.max(0, loops));
    while (cells.length < treeRooms) {
        if (!place(1)) break;
    }
    // Then the rooms that close a way round. Where none is available - a mine
    // grown into a corridor has nowhere to put one - this simply stops, and
    // the report says how many it managed.
    while (cells.length < count) {
        if (!place(2) && !place(1)) break;
    }
    return cells;
}

// Every pair of occupied cells that touch face to face. All of them become
// portals; see the note on `growCells`.
function latticeEdges(cells) {
    const index = new Map(cells.map((c, i) => [key(c), i]));
    const edges = [];
    for (let i = 0; i < cells.length; ++i) {
        for (let s = 0; s < STEPS.length; ++s) {
            const other = index.get(key(add(cells[i], STEPS[s].delta)));
            if (other === undefined || other <= i) continue;
            edges.push({ a: i, b: other, step: s });
        }
    }
    return edges;
}

// A spanning tree over those edges, plus the leftovers.
//
// The tree is what gets extruded, because extrusion is the only operation that
// can make a room that does not exist yet. The leftovers are welded with
// `joinSides`, and each one is a way round - which is the difference the
// shipped levels actually show: mine01 has eight independent cycles,
// MineEntrance has none, and it is the single biggest thing separating a mine
// from a corridor.
function spanningTree(rng, cellCount, edges) {
    const parent = [...Array(cellCount).keys()];
    const find = (x) => (parent[x] === x ? x : (parent[x] = find(parent[x])));

    const shuffled = edges.slice();
    for (let i = shuffled.length - 1; i > 0; --i) {
        const j = Math.floor(rng() * (i + 1));
        [shuffled[i], shuffled[j]] = [shuffled[j], shuffled[i]];
    }

    const tree = [];
    const spare = [];
    for (const edge of shuffled) {
        const ra = find(edge.a);
        const rb = find(edge.b);
        if (ra === rb) {
            spare.push(edge);
            continue;
        }
        parent[ra] = rb;
        tree.push(edge);
    }
    return { tree, spare };
}

// --- realising it ----------------------------------------------------------

// Extrude the tree, then weld the loops.
//
// Cells are visited breadth first from the root so that every room's parent
// already exists when it is grown from it. `segmentOf` maps a cell to the
// segment index the level ended up giving it, which is not the cell's own
// index: segment 0 is the starter room `newLevel` made.
function buildRooms(level, cells, tree, loops) {
    const children = cells.map(() => []);
    for (const edge of tree) {
        children[edge.a].push({ to: edge.b, step: edge.step });
        children[edge.b].push({ to: edge.a, step: OPPOSITE_STEP[edge.step] });
    }

    const segmentOf = new Array(cells.length).fill(-1);
    const parentOf = new Array(cells.length).fill(-1);
    const depth = new Array(cells.length).fill(0);
    segmentOf[0] = 0;

    const order = [0];
    for (let head = 0; head < order.length; ++head) {
        const cell = order[head];
        for (const link of children[cell]) {
            if (segmentOf[link.to] >= 0) continue;
            const side = STEPS[link.step].side;
            segmentOf[link.to] =
                extrudeSide(level, segmentOf[cell], side, SIDE_PITCH[side]);
            parentOf[link.to] = cell;
            depth[link.to] = depth[cell] + 1;
            order.push(link.to);
        }
    }

    // The leftover edges. A join that fails leaves two rooms face to face with
    // no portal between them, which the closure check rejects - so this is
    // counted and the caller re-rolls rather than shipping it.
    let closed = 0;
    const refused = [];
    for (const edge of loops) {
        try {
            joinSides(level, segmentOf[edge.a], STEPS[edge.step].side,
                      segmentOf[edge.b], STEPS[OPPOSITE_STEP[edge.step]].side, 0.5);
            ++closed;
        } catch (e) {
            refused.push(e.message);
        }
    }

    return { segmentOf, parentOf, depth, order, closed, refused, wanted: loops.length };
}

// --- zones -----------------------------------------------------------------

// Mark runs of rooms as rock or as built.
//
// In runs, not per room. The material language in the levels that already
// exist is a sequence - MineEntrance goes grate, then rock, then panel - and
// alternating room by room would read as noise rather than as somewhere that
// changes. A run is three to six rooms of the walk order.
function assignZones(rng, order, rockFraction) {
    const rock = new Array(order.length).fill(false);
    let isRock = rockFraction >= 0.5;
    let left = 0;

    for (const cell of order) {
        if (left === 0) {
            // Alternating, with the FRACTION setting how long each stretch
            // runs rather than a coin toss per stretch. A mine this size holds
            // about five stretches, and five coin tosses land nowhere near the
            // asked-for share often enough that the setting would visibly not
            // mean what it says.
            isRock = !isRock;
            const span = 5 + Math.floor(rng() * 5);
            const rockPart = Math.round(span * rockFraction);
            left = Math.max(1, isRock ? rockPart : span - rockPart);
        }
        rock[cell] = isRock;
        --left;
    }
    return rock;
}

// Push the rock rooms around.
//
// A vertex moves only when EVERY room that shares it is rock. That is what
// keeps the built stretches square: the boundary between a corridor and a cave
// is a face both of them own, so it stays on the lattice and the cave grows
// rough away from it. It also means this can run after the joins without
// taking them apart, because a shared vertex is one index and moving it moves
// both rooms at once.
function roughen(level, rng, segmentOf, rock, roughness, options) {
    const amount = roughness * ROOM;

    const rockSegments = new Set();
    const allSegments = new Set();
    for (let cell = 0; cell < segmentOf.length; ++cell) {
        if (segmentOf[cell] < 0) continue;
        allSegments.add(segmentOf[cell]);
        if (rock[cell]) rockSegments.add(segmentOf[cell]);
    }

    const owners = new Map();
    for (let si = 0; si < level.segments.length; ++si) {
        for (const v of level.segments[si].verts) {
            if (!owners.has(v)) owners.set(v, []);
            owners.get(v).push(si);
        }
    }

    // Every vertex moves a little; the rock ones move a lot.
    //
    // The small one is not decoration. A segment that is a PERFECT cube has
    // its centre exactly on the diagonal through four of its edges, and the
    // game's raycast - Moller-Trumbore with no edge slack, deliberately, so a
    // wall never over-reports - can miss both triangles of both faces there
    // and report a ray leaving a closed mine. Neither hand-built level trips
    // it because neither has a perfect cube in it. A generator that emits
    // nothing else trips it on the first ray `--collision-test` fires.
    const settle = ROOM * (options.settle ?? 0.02);

    const updates = [];
    for (const [vertex, segments] of owners) {
        const rough = segments.every((si) => rockSegments.has(si));
        const spread = rough ? amount : settle;
        const jitter = [0, 1, 2].map(() => (rng() * 2 - 1) * spread);
        updates.push([vertex, add(level.vertices[vertex], jitter)]);
    }
    if (updates.length > 0) moveVertices(level, updates);
    return updates.length;
}

// Rock takes the rock tile. Built rooms alternate panel and grate by run, so a
// built stretch is one material rather than a chequerboard.
function paint(level, rng, segmentOf, rock) {
    let builtTile = rng() < 0.5 ? WALL_PANEL : WALL_GRATE;
    let left = 0;
    for (let cell = 0; cell < segmentOf.length; ++cell) {
        const si = segmentOf[cell];
        if (si < 0) continue;
        if (!rock[cell]) {
            if (left === 0) {
                builtTile = builtTile === WALL_PANEL ? WALL_GRATE : WALL_PANEL;
                left = 3 + Math.floor(rng() * 4);
            }
            --left;
        }
        const tile = rock[cell] ? WALL_ROCK : builtTile;
        for (let side = 0; side < 6; ++side) {
            if (level.segments[si].sides[side].neighbour >= 0) continue;
            setTexture(level, si, side, tile);
        }
    }
}

// --- content ---------------------------------------------------------------

// A point inside a room, pulled off the exact centre so two things placed in
// one room do not sit inside each other.
function spot(level, si, rng, spread = 0.28) {
    const centre = segmentCentre(level, si);
    const offset = [0, 1, 2].map(() => (rng() * 2 - 1) * spread * ROOM);
    return add(centre, offset);
}

// The key chain, and the reason this generator can promise a level is finishable.
//
// Every door it locks sits on an edge of the SPANNING TREE, along the path
// from the start to the exit. Removing a tree edge splits the tree in two, so
// "before that door" and "beyond that door" are exact sets rather than a
// guess. Key j is placed in the region between door j-1 and door j, which is
// reachable with keys 1..j-1 in hand and does not require key j. Locking a
// loop edge instead would prove nothing - you could go round.
function lockDoors(level, rng, plan, keys) {
    const { segmentOf, parentOf, depth, order } = plan;
    if (keys <= 0) return { doors: [], exitCell: order[order.length - 1] };

    // The deepest room is the far end of the mine, and the path to it is the
    // spine the chain gets hung on.
    let exitCell = order[0];
    for (const cell of order) if (depth[cell] > depth[exitCell]) exitCell = cell;

    const path = [];
    for (let cell = exitCell; cell !== -1; cell = parentOf[cell]) path.unshift(cell);
    if (path.length < 3) return { doors: [], exitCell };

    // Evenly spaced along the spine, skipping the first edge so the player is
    // never locked in at the door of the room they spawned in.
    const usable = Math.min(keys, path.length - 2);
    const chosen = [];
    for (let j = 1; j <= usable; ++j) {
        const at = Math.floor((path.length - 1) * j / (usable + 1));
        if (at >= 1 && !chosen.includes(at)) chosen.push(at);
    }

    // Which side of the tree each room falls on, once an edge is cut.
    const beyond = (cutChild) => {
        const set = new Set([cutChild]);
        let grew = true;
        while (grew) {
            grew = false;
            for (const cell of order) {
                if (set.has(cell) || parentOf[cell] === -1) continue;
                if (set.has(parentOf[cell])) {
                    set.add(cell);
                    grew = true;
                }
            }
        }
        return set;
    };

    const doors = [];
    let previous = null;
    for (let j = 0; j < chosen.length; ++j) {
        const child = path[chosen[j]];
        const parent = parentOf[child];
        const far = beyond(child);

        // The region this key lives in: past the previous door, short of this
        // one. For the first key that is everything before it.
        const region = order.filter((cell) => !far.has(cell) &&
                                              (previous === null || previous.has(cell)));
        if (region.length === 0) break;

        const step = STEPS.find((s) => segmentOf[child] ===
            level.segments[segmentOf[parent]].sides[s.side].neighbour);
        if (!step) break;

        try {
            addDoor(level, segmentOf[parent], step.side, j);
        } catch {
            break;
        }
        const host = pick(rng, region);
        addPickup(level, PICKUP_KEY[j], spot(level, segmentOf[host], rng));
        doors.push({ key: j, segment: segmentOf[parent], side: step.side, keyIn: segmentOf[host] });
        previous = far;
    }

    return { doors, exitCell };
}

function furnish(level, rng, plan, rock, options) {
    const { segmentOf, depth, order } = plan;
    const deepest = Math.max(...order.map((cell) => depth[cell])) || 1;

    // Lights. A built room always has one, because built means someone put
    // lights in. A rock room usually does not, and that is the mine being a
    // mine rather than the generator forgetting.
    for (const cell of order) {
        const si = segmentOf[cell];
        if (si < 0) continue;
        const built = !rock[cell];
        if (!built && rng() > options.litRock) continue;
        const centre = segmentCentre(level, si);
        const light = level.lights[
            addLight(level, [centre[0], centre[1] + ROOM * 0.3, centre[2]], si)];
        light.radius = built ? ROOM * 1.6 : ROOM * 1.1;
        light.intensity = built ? 2.0 : 1.4;
        // Cold where someone wired it, warm where someone hung a lamp.
        light.color = built ? [0.86, 0.92, 1.0] : [1.0, 0.86, 0.62];
    }

    // The key chain, then the way out at the end of it.
    const { doors, exitCell } = lockDoors(level, rng, plan, options.keys);

    const reactorCell = order.reduce(
        (best, cell) => (cell !== exitCell && depth[cell] > depth[best] ? cell : best), order[0]);
    addObject(level, 'reactor', segmentCentre(level, segmentOf[reactorCell]));
    setExit(level, segmentCentre(level, segmentOf[exitCell]));

    // Robots, weighted towards the deep end. A brute in the room you spawn in
    // is not difficulty, it is a coin toss.
    const spawnable = order.filter((cell) => cell !== order[0] && cell !== reactorCell);
    let placed = 0;
    for (const cell of spawnable) {
        if (placed >= options.robots) break;
        const share = depth[cell] / deepest;
        if (rng() > 0.25 + share * 0.5) continue;
        const type = share > 0.66 ? pick(rng, ['gunner', 'brute', 'gunner'])
                   : share > 0.33 ? pick(rng, ['drone', 'gunner'])
                   : 'drone';
        addObject(level, type, spot(level, segmentOf[cell], rng));
        ++placed;
    }

    // Columns, in rock rooms only - a pillar in a corridor is an obstacle, in
    // a cave it is cover.
    //
    // Scaled to the room rather than dropped in at 1.0. `pillar.obj` is 18
    // units tall and the lattice is 17, so a default-scaled column pokes
    // through the floor and the ceiling of every room it stands in - and
    // object collision is gated to the object's own segment, so the part that
    // left the room is drawn, casts shadows, and stops nothing. That is the
    // fault `docs/KNOWN_ISSUES.md` describes, and a generator has no excuse
    // for producing it on purpose: `--collision-test` caught this as a body
    // ending up inside a column.
    const PILLAR_HEIGHT = 18;
    for (const cell of order) {
        if (!rock[cell] || rng() > 0.18) continue;
        const si = segmentOf[cell];
        const centre = segmentCentre(level, si);
        const corners = level.segments[si].verts.map((v) => level.vertices[v]);
        const room = Math.max(...corners.map((c) => c[1])) - Math.min(...corners.map((c) => c[1]));
        const fit = Math.min(1, (room * 0.78) / PILLAR_HEIGHT);
        if (fit < 0.35) continue;   // too low a room to be worth a column
        level.objects[addObject(level, 'pillar', centre)].scale =
            Math.round(fit * 100) / 100;
    }

    // Supplies, thinner the deeper you go, so the run gets harder rather than
    // just longer.
    for (const cell of order) {
        if (cell === order[0] || rng() > 0.3) continue;
        const share = depth[cell] / deepest;
        const kind = share > 0.5 ? pick(rng, [PICKUP_SHIELD, PICKUP_ENERGY, PICKUP_CONCUSSION])
                                 : pick(rng, [PICKUP_ENERGY, PICKUP_FLARES, PICKUP_SHIELD]);
        addPickup(level, kind, spot(level, segmentOf[cell], rng));
    }

    return { doors, exitCell, robots: placed };
}

// --- the loop ---------------------------------------------------------------

// One attempt. Returns the level and what it turned out to contain.
function attempt(name, options, seed) {
    const rng = makeRng(seed);
    const level = newLevel(name, ROOM);
    // `newLevel` makes a cube; stretch it to the lattice's own box so the room
    // the mine grows from matches every room grown from it.
    moveVertices(level, level.vertices.map((v, i) => [i, [v[0], v[1] / ROOM * CELL[1],
                                                          v[2] / ROOM * CELL[2]]]));
    level.comment = `Generated in the level editor. seed ${seed}, ${options.rooms} rooms.`;

    const cells = growCells(rng, Math.max(2, options.rooms), options.loops);
    const edges = latticeEdges(cells);
    const { tree, spare } = spanningTree(rng, cells.length, edges);

    // Every leftover edge, not a chosen subset: an adjacency left unwelded is
    // a doubled wall, not a wall. The count of them IS the loop count, and it
    // was fixed when the cells were placed.
    const plan = buildRooms(level, cells, tree, spare);
    const rock = assignZones(rng, plan.order, options.rockFraction);
    roughen(level, rng, plan.segmentOf, rock, options.roughness, options);
    paint(level, rng, plan.segmentOf, rock);

    // The player starts in the room the mine grew from, looking down -Z.
    level.start.segment = 0;
    level.start.position = segmentCentre(level, 0);

    const content = furnish(level, rng, plan, rock, options);

    return {
        level,
        report: {
            seed,
            rooms: level.segments.length,
            loopsAsked: options.loops,
            loopsClosed: plan.closed,
            loopsRefused: plan.refused,
            rockRooms: plan.order.filter((c) => rock[c]).length,
            builtRooms: plan.order.filter((c) => !rock[c]).length,
            lights: level.lights.length,
            robots: content.robots,
            pillars: level.objects.filter((o) => o.type === 'pillar').length,
            pickups: level.pickups.length,
            doors: content.doors.length,
            keys: content.doors.map((d) => d.key),
        },
    };
}

// Generate a mine, re-rolling until one validates.
//
// The re-roll is the point. Roughening a room can fold it through itself, and
// a folded segment is a level the game will not load; rather than trying to
// prove in advance that a displacement is safe, this asks the editor's own
// validator - the same rules the game's loader enforces - and throws the
// attempt away if it objects. `segment-overlap` is a warning rather than an
// error and is tolerated in small numbers, because two rock rooms sharing a
// little space is what a cave looks like.
//
// `attempts` bounds the cost: every roll is milliseconds, and asking for a
// mine that cannot exist (forty rooms with thirty loops) should stop rather
// than spin.
export function generateMine(name, overrides = {}, attempts = 24) {
    const options = { ...GENERATOR_DEFAULTS, ...overrides };
    const base = (options.seed >>> 0) || (Math.floor(Math.random() * 0xffffffff) >>> 0);

    const rejected = [];
    for (let tries = 0; tries < attempts; ++tries) {
        const seed = ((base + tries * 0x9e3779b1) >>> 0) || 1;
        let candidate;
        try {
            candidate = attempt(name, options, seed);
        } catch (e) {
            rejected.push(`seed ${seed}: ${e.message}`);
            continue;
        }

        const result = validateLevel(candidate.level);
        const errors = result.findings.filter((f) => f.severity === 'error');
        const overlaps = result.findings.filter((f) => f.rule === 'segment-overlap');
        if (errors.length > 0) {
            rejected.push(`seed ${seed}: ${errors[0].message}`);
            continue;
        }
        if (overlaps.length > candidate.level.segments.length * 0.15) {
            rejected.push(`seed ${seed}: ${overlaps.length} rooms overlap`);
            continue;
        }
        // Every room has to contain its own middle.
        //
        // Roughening moves eight corners independently, and enough of that
        // makes a room whose averaged centre is no longer inside it. Nothing
        // in the structural rules objects - every index is still valid and the
        // surface is still closed - but the game fires its rays from segment
        // centres, so the first thing `--collision-test` reports is a ray that
        // "stopped at 0.000", and `--light-test` starts disagreeing with its
        // own brute force. Checked with the same predicate `findSegment` uses,
        // which is the one the game will apply.
        const inverted = [];
        for (let si = 0; si < candidate.level.segments.length; ++si) {
            if (!insideSegment(candidate.level, si, candidate.level.segments[si].center, 0.25)) {
                inverted.push(si);
            }
        }
        if (inverted.length > 0) {
            rejected.push(`seed ${seed}: ${inverted.length} room(s) do not contain their own centre`);
            continue;
        }

        // The same claim `--world-test` makes: one closed surface of genus 0.
        // Checked here as well as prevented during growth, because prevention
        // reasons about the lattice and this measures the mesh that came out
        // of it, and only the second one is the thing the game loads.
        const closure = result.closure;
        const euler = closure
            ? closure.weldedVertices - closure.edges + closure.triangles : 2;
        if (euler !== 2) {
            rejected.push(`seed ${seed}: wall surface has Euler characteristic ${euler}, not 2`);
            continue;
        }
        candidate.report.euler = euler;

        candidate.report.attempts = tries + 1;
        candidate.report.rejected = rejected;
        candidate.report.overlaps = overlaps.length;
        candidate.report.closure = result.closure;
        return candidate;
    }

    throw new Error(`could not generate a mine that validates in ${attempts} tries. ` +
                    `Last objection: ${rejected[rejected.length - 1] ?? 'unknown'}`);
}
