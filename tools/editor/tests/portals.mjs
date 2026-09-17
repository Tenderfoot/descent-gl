// What a click on a portal resolves to.
//
// A portal has no wall triangles: the loader skips any side with a neighbour
// and rebuildDerived() does the same, because the game does. So the translucent
// quad in the viewport is the only thing there is to hit, and until it carried
// a record a ray aimed at one passed straight through the opening and selected
// the far wall of the next room. That left every portal tool unreachable by
// clicking - a door goes on a portal, and so does Split portal.
//
// The picking itself is three.js and lives in view.js, which these cannot
// import. What they check is the part that can be wrong without a browser
// noticing: the record a hit is resolved against, and which of a portal's TWO
// ends that hit means.
//
//   node tools/editor/tests/portals.mjs

import fs from 'node:fs';

import { SIDE_COUNT, SIDE_NAMES, SIDE_VERTICES } from '../src/format.js';
import { addDoor } from '../src/entities.js';
import { oppositeSide, portalFaces, portalFacing, sub } from '../src/geometry.js';
import { parseLevel } from '../src/level.js';

const LEVELS = ['tools/editor/testdata/base.json', 'assets/levels/mine01.json',
                'assets/levels/test01.json'];

let failures = 0;
const report = (ok, label, detail = '') => {
    if (!ok) ++failures;
    console.log(`${ok ? 'ok  ' : 'FAIL'}  ${label}${detail ? `\n        ${detail}` : ''}`);
};

const read = (file) => parseLevel(fs.readFileSync(file, 'utf8'), file);

// Every side of every segment that leads somewhere, counted from both ends.
function portalSideCount(level) {
    let n = 0;
    for (const seg of level.segments) for (const sd of seg.sides) if (sd.neighbour >= 0) ++n;
    return n;
}

for (const file of LEVELS) {
    const level = read(file);
    const faces = portalFaces(level);
    console.log(`\n${file} - ${level.segments.length} segments, ${faces.length} portals`);

    // --- one record per portal, not per side -------------------------------

    report(faces.length * 2 === portalSideCount(level),
           'every portal appears exactly once',
           `${faces.length} records for ${portalSideCount(level)} portal sides`);

    let owned = 0;
    let reciprocal = 0;
    let shared = 0;
    let carriesDoor = 0;
    const seen = new Set();

    for (const face of faces) {
        if (face.segment < face.neighbour) ++owned;
        seen.add(`${face.segment}:${face.side}`);

        // The reciprocal side has to be the one that looks back. Getting this
        // wrong is what would make a click on one end of a portal select some
        // unrelated side of the room on the other.
        const back = level.segments[face.neighbour]?.sides[face.neighbourSide];
        if (back && back.neighbour === face.segment) ++reciprocal;

        // The format's rule, and the reason join rewrites indices rather than
        // moving vertices: the two ends of a portal are the SAME four vertices,
        // not four coincident positions.
        const ends = [[face.segment, face.side], [face.neighbour, face.neighbourSide]]
            .map(([si, side]) => SIDE_VERTICES[side]
                .map((corner) => level.segments[si].verts[corner]).sort().join(','));
        if (ends[0] === ends[1]) ++shared;

        if (face.door >= 0) ++carriesDoor;
    }

    report(owned === faces.length, 'each is held by the lower-numbered segment',
           `${owned} of ${faces.length}`);
    report(seen.size === faces.length, 'no side is listed twice');
    report(reciprocal === faces.length, 'the far end of each looks back',
           `${reciprocal} of ${faces.length}`);
    report(shared === faces.length, 'both ends name the same four vertices',
           `${shared} of ${faces.length}`);
    report(carriesDoor === level.doors.length,
           'every door is on a record, so a door can be clicked',
           `${carriesDoor} records carry a door, level has ${level.doors.length}`);

    // --- the reason a record is needed at all ------------------------------

    const wallOnPortal = level.walls.filter((wall) =>
        level.segments[wall.segment].sides[wall.side].neighbour >= 0);
    report(wallOnPortal.length === 0,
           'no portal has wall triangles, so the quad is all there is to hit',
           wallOnPortal.length === 0 ? '' : `${wallOnPortal.length} triangles on portal sides`);

    // --- which end a hit means ---------------------------------------------

    // Looking at a portal from inside one of its two segments has to select
    // that segment's side of it, whichever way round the record was built. The
    // direction a click carries is the camera ray, so the test uses the one
    // from a segment's centre to the middle of the portal.
    let facing = 0;
    for (const face of faces) {
        const middle = face.corners.reduce((a, c) => [a[0] + c[0] / 4, a[1] + c[1] / 4, a[2] + c[2] / 4],
                                           [0, 0, 0]);
        const near = portalFacing(face, sub(middle, level.segments[face.segment].center));
        const far = portalFacing(face, sub(middle, level.segments[face.neighbour].center));
        const ok = near.segment === face.segment && near.side === face.side &&
                   far.segment === face.neighbour && far.side === face.neighbourSide;
        if (ok) ++facing;
        else if (facing === 0) {
            report(false, 'a hit resolves to the end it was seen from',
                   `portal ${face.segment}/${SIDE_NAMES[face.side]} - ` +
                   `near gave ${near.segment}/${SIDE_NAMES[near.side]}, ` +
                   `far gave ${far.segment}/${SIDE_NAMES[far.side]}`);
        }
    }
    if (facing === faces.length) {
        report(true, 'a hit resolves to the end it was seen from, from both ends',
               `${facing} portals`);
    }
}

// --- a door that came from the file -----------------------------------------

// The reason this is here rather than in entities.mjs: it is only reachable by
// clicking a portal. A door in the doors array puts no `door` on either side of
// the file, and until parseLevel rewired them the editor saw an empty portal
// and offered to add a second door on top of the first.
{
    const level = read('assets/levels/mine01.json');
    let wired = 0;
    for (let di = 0; di < level.doors.length; ++di) {
        const door = level.doors[di];
        const near = level.segments[door.segment].sides[door.side];
        const far = level.segments[near.neighbour]?.sides[oppositeSide(level, door.segment, door.side)];
        if (near.door === di && far?.door === di) ++wired;
    }
    report(wired === level.doors.length && level.doors.length > 0,
           'a door in the file is wired onto both ends of its portal',
           `${wired} of ${level.doors.length}`);

    let refused = false;
    try { addDoor(level, level.doors[0].segment, level.doors[0].side); } catch { refused = true; }
    report(refused, 'so clicking that portal cannot put a second door on top of it');
}

// --- a level whose graph is one-sided ---------------------------------------

// validate.js reports this as an error, but the editor still has to draw and
// select on a level that is mid-edit or hand-written. Falling back to the end
// that does exist keeps a click selecting something rather than nothing.
{
    const level = read(LEVELS[0]);
    const face = portalFaces(level)[0];
    for (let os = 0; os < SIDE_COUNT; ++os) {
        if (level.segments[face.neighbour].sides[os].neighbour === face.segment) {
            level.segments[face.neighbour].sides[os].neighbour = -1;
        }
    }

    const broken = portalFaces(level).find((f) => f.segment === face.segment && f.side === face.side);
    report(oppositeSide(level, face.segment, face.side) === -1,
           'a one-sided portal has no reciprocal side');
    report(broken?.neighbourSide === -1, 'and the record says so rather than guessing');

    const middle = broken.corners.reduce((a, c) => [a[0] + c[0] / 4, a[1] + c[1] / 4, a[2] + c[2] / 4],
                                         [0, 0, 0]);
    const from = portalFacing(broken, sub(middle, level.segments[broken.neighbour].center));
    report(from.segment === broken.segment && from.side === broken.side,
           'and a hit from either side still selects the end that exists');
}

console.log(`\n${failures === 0 ? 'portals: PASS' : `portals: FAIL (${failures})`}`);
process.exit(failures === 0 ? 0 : 1);
