// A group transform, as plain arrays.
//
// The gizmo is a three.js object and three.js lives only in view.js, so the
// maths that moves a selection lives here instead: given a pivot, a
// translation, a rotation and a scale, every selected point moves by
//
//     p' = pivot + translation + R * F * S * F^-1 * (p - pivot)
//
// which is the identity when the gizmo has not been dragged. F is the frame
// the gizmo's handles were drawn in: identity in world space, the selection's
// own axes in local space. Scale acts along its axes; R is how far the gizmo
// has turned from it, and is a world rotation either way.
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

export const conjugateQuat = (q) => [-q[0], -q[1], -q[2], q[3]];

// --- frames ----------------------------------------------------------------
//
// A frame is a rotation, [x, y, z, w], taking world axes onto a selection's
// own. Local space parks the gizmo in one, so its arrows run along a wall
// rather than along the world.

const dot3 = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross3 = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                          a[0] * b[1] - a[1] * b[0]];
const unit = (a) => {
    const len = Math.sqrt(dot3(a, a));
    return len < 1e-9 ? null : a.map((n) => n / len);
};

// The rotation whose columns are the orthonormal x, y, z.
function quatFromBasis(x, y, z) {
    const [m00, m10, m20] = x;
    const [m01, m11, m21] = y;
    const [m02, m12, m22] = z;
    const trace = m00 + m11 + m22;
    let q;
    if (trace > 0) {
        const s = 0.5 / Math.sqrt(trace + 1);
        q = [(m21 - m12) * s, (m02 - m20) * s, (m10 - m01) * s, 0.25 / s];
    } else if (m00 > m11 && m00 > m22) {
        const s = 2 * Math.sqrt(1 + m00 - m11 - m22);
        q = [0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s];
    } else if (m11 > m22) {
        const s = 2 * Math.sqrt(1 + m11 - m00 - m22);
        q = [(m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s];
    } else {
        const s = 2 * Math.sqrt(1 + m22 - m00 - m11);
        q = [(m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s];
    }
    const len = Math.hypot(...q);
    return q.map((n) => n / len);
}

// A frame whose x runs along `xDir` and whose y is `yHint` made square to it.
// Null when either is degenerate or the two are parallel.
export function frameFromAxes(xDir, yHint) {
    const x = unit(xDir);
    if (!x) return null;
    const y = unit(yHint.map((n, k) => n - x[k] * dot3(yHint, x)));
    if (!y) return null;
    return quatFromBasis(x, y, cross3(x, y));
}

// A frame for an edge: x along it, y as near world up as the edge allows.
export function edgeFrame(a, b) {
    const along = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
    return frameFromAxes(along, [0, 1, 0]) ?? frameFromAxes(along, [0, 0, 1]);
}

// A frame fitted to a loose set of points: x along the direction they spread
// furthest, z along the one they spread least - a flat set's normal. Signs are
// chosen so each axis leans positive along the world axis it is nearest,
// which keeps the frame from flipping between two drags of the same shape.
//
// Symmetric 3x3 eigenvectors by Jacobi rotation: a handful of sweeps, and
// no dependency for one small matrix.
export function spreadFrame(points) {
    if (points.length < 2) return null;
    const c = centroid(points);
    const m = [[0, 0, 0], [0, 0, 0], [0, 0, 0]];
    for (const p of points) {
        const d = [p[0] - c[0], p[1] - c[1], p[2] - c[2]];
        for (let i = 0; i < 3; ++i) for (let j = 0; j < 3; ++j) m[i][j] += d[i] * d[j];
    }
    const v = [[1, 0, 0], [0, 1, 0], [0, 0, 1]];   // columns are the eigenvectors
    for (let sweep = 0; sweep < 16; ++sweep) {
        let off = 0;
        for (let i = 0; i < 3; ++i) for (let j = i + 1; j < 3; ++j) off += m[i][j] * m[i][j];
        if (off < 1e-18) break;
        for (let p = 0; p < 3; ++p) {
            for (let q = p + 1; q < 3; ++q) {
                if (Math.abs(m[p][q]) < 1e-12) continue;
                const theta = (m[q][q] - m[p][p]) / (2 * m[p][q]);
                const t = Math.sign(theta || 1) / (Math.abs(theta) + Math.sqrt(theta * theta + 1));
                const cs = 1 / Math.sqrt(t * t + 1);
                const sn = t * cs;
                for (let k = 0; k < 3; ++k) {
                    const mkp = m[k][p];
                    const mkq = m[k][q];
                    m[k][p] = cs * mkp - sn * mkq;
                    m[k][q] = sn * mkp + cs * mkq;
                }
                for (let k = 0; k < 3; ++k) {
                    const mpk = m[p][k];
                    const mqk = m[q][k];
                    m[p][k] = cs * mpk - sn * mqk;
                    m[q][k] = sn * mpk + cs * mqk;
                }
                for (let k = 0; k < 3; ++k) {
                    const vkp = v[k][p];
                    const vkq = v[k][q];
                    v[k][p] = cs * vkp - sn * vkq;
                    v[k][q] = sn * vkp + cs * vkq;
                }
            }
        }
    }
    const order = [0, 1, 2].sort((a, b) => m[b][b] - m[a][a]);
    if (m[order[0]][order[0]] < 1e-12) return null;   // every point in one place
    const axis = (i) => {
        const e = [v[0][i], v[1][i], v[2][i]];
        const big = e.reduce((best, n, k) => (Math.abs(n) > Math.abs(e[best]) ? k : best), 0);
        return e[big] < 0 ? e.map((n) => -n) : e;
    };
    return frameFromAxes(axis(order[0]), axis(order[1]));
}

// The transform the gizmo describes, with every field optional so that a
// translate-only drag reads as one.
export function groupTransform({ pivot = [0, 0, 0], translate = [0, 0, 0],
                                 rotation = IDENTITY_QUAT, scale = [1, 1, 1],
                                 frame = IDENTITY_QUAT } = {}) {
    const safe = sanitiseScale(scale);
    return { pivot, translate, rotation, scale: safe, frame };
}

export function transformPoint(t, p) {
    const frame = t.frame ?? IDENTITY_QUAT;
    const offset = [p[0] - t.pivot[0], p[1] - t.pivot[1], p[2] - t.pivot[2]];
    const local = rotateVector(conjugateQuat(frame), offset);
    const scaled = rotateVector(frame,
        [local[0] * t.scale[0], local[1] * t.scale[1], local[2] * t.scale[2]]);
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
