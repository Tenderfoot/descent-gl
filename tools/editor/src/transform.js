// A group transform, as plain arrays.
//
// The gizmo is a three.js object and three.js lives only in view.js, so the
// maths that moves a selection lives here instead: given a pivot, a
// translation, a rotation and a scale, every selected point moves by
//
//     p' = pivot + translation + R * (S * (p - pivot))
//
// which is the identity when the gizmo has not been dragged.
//
// Two rules the shape of this function encodes:
//
//  1. It is applied to the positions recorded at drag START, never to the
//     positions it last produced. TransformControls reports absolute state
//     every frame, so integrating it would compound rounding over the few
//     hundred frames of a drag and leave a group slightly deformed by the act
//     of having been moved.
//  2. It moves points. It does not decide what a point IS - a vertex, a
//     light, a lamp's anchor - because that is the caller's business and
//     mixing the two is how a transform ends up with a case per entity kind.

// Quaternions here are [x, y, z, w], which is three.js's order and therefore
// the order the gizmo hands over. The level file stores them [w, x, y, z]; these
// two are the only place that conversion happens outside view.js.
export const fromLevelQuat = (q) => [q[1], q[2], q[3], q[0]];
export const toLevelQuat = (q) => [q[3], q[0], q[1], q[2]];

export const IDENTITY_QUAT = [0, 0, 0, 1];

// A scale factor no group transform will go below.
//
// Nothing in either validator catches an inverted segment: there is no winding
// rule, so a segment mirrored by a negative scale references the same vertices
// in the same order, passes every structural check, and renders with its
// normals pointing out of the mine. The symptom is a room that is lit from
// nowhere and that the ship falls through, and the cause is a scale handle
// dragged one pixel too far. So the handle cannot express it.
export const MIN_SCALE = 1e-3;

export function sanitiseScale(scale) {
    return scale.map((s) => (Math.abs(s) < MIN_SCALE ? MIN_SCALE : Math.abs(s)));
}

// Mean of the points. The pivot for a rotate or a scale, and the point the
// gizmo is parked at.
//
// The mean rather than the centre of the bounding box: for the two selections
// that matter - a side's four corners and a segment's eight - they are the same
// point, and the mean does not lurch when a far-off vertex joins the selection.
export function centroid(points) {
    if (points.length === 0) return [0, 0, 0];
    const sum = [0, 0, 0];
    for (const p of points) {
        sum[0] += p[0];
        sum[1] += p[1];
        sum[2] += p[2];
    }
    return sum.map((n) => n / points.length);
}

// q * v, for q as [x, y, z, w]. The usual v + 2q_w(q_v x v) + 2q_v x (q_v x v),
// written out rather than built from a cross-product helper because this runs
// once per selected point per frame of a drag.
export function rotateVector(q, v) {
    const [qx, qy, qz, qw] = q;
    const [x, y, z] = v;
    const tx = 2 * (qy * z - qz * y);
    const ty = 2 * (qz * x - qx * z);
    const tz = 2 * (qx * y - qy * x);
    return [
        x + qw * tx + (qy * tz - qz * ty),
        y + qw * ty + (qz * tx - qx * tz),
        z + qw * tz + (qx * ty - qy * tx),
    ];
}

// a then b, i.e. b * a. Used to turn an object's own orientation by the group's
// rotation: a cluster of scenery rotated as a group should face the way it is
// turned, not keep staring in the direction it was placed in.
export function multiplyQuat(b, a) {
    const [bx, by, bz, bw] = b;
    const [ax, ay, az, aw] = a;
    return [
        bw * ax + bx * aw + by * az - bz * ay,
        bw * ay - bx * az + by * aw + bz * ax,
        bw * az + bx * ay - by * ax + bz * aw,
        bw * aw - bx * ax - by * ay - bz * az,
    ];
}

// The transform the gizmo describes, with every field optional so that a
// translate-only drag reads as one.
export function groupTransform({ pivot = [0, 0, 0], translate = [0, 0, 0],
                                 rotation = IDENTITY_QUAT, scale = [1, 1, 1] } = {}) {
    const safe = sanitiseScale(scale);
    return { pivot, translate, rotation, scale: safe };
}

export function transformPoint(t, p) {
    const local = [p[0] - t.pivot[0], p[1] - t.pivot[1], p[2] - t.pivot[2]];
    const scaled = [local[0] * t.scale[0], local[1] * t.scale[1], local[2] * t.scale[2]];
    const turned = rotateVector(t.rotation, scaled);
    return [
        t.pivot[0] + t.translate[0] + turned[0],
        t.pivot[1] + t.translate[1] + turned[1],
        t.pivot[2] + t.translate[2] + turned[2],
    ];
}

export function transformPoints(t, points) {
    return points.map((p) => transformPoint(t, p));
}

// A direction carried by something being transformed: a spot's aim, a lamp's
// swing axis. Rotated, never translated, and not scaled either - a non-uniform
// scale would change its length and a direction has none.
export function transformDirection(t, d) {
    return rotateVector(t.rotation, d);
}

// True when the transform would not move anything, so a drag that ended where
// it started records no undo entry and no edit.
export function isIdentityTransform(t, epsilon = 1e-9) {
    const near = (a, b) => Math.abs(a - b) <= epsilon;
    return near(t.translate[0], 0) && near(t.translate[1], 0) && near(t.translate[2], 0)
        && near(t.scale[0], 1) && near(t.scale[1], 1) && near(t.scale[2], 1)
        && near(Math.abs(t.rotation[3]), 1);
}
