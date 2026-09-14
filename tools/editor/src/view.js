// Turning a parsed level into something on screen.
//
// This is the only module that knows about three.js. Everything it draws comes
// from level.walls, which geometry.js built with the game's own triangulation,
// so a wall on screen is a wall the game will build - not an approximation
// of one.
//
// What this is not: the renderer. There are no shadows here, no falloff, no
// reversed-Z. Lights are drawn as gizmos and reach previews. The question
// "how does this look" is answered by the game, with --segment N --screenshot.

import * as THREE from 'three';

import { SIDE_COUNT, SIDE_NAMES, WALL_TILE_NAMES } from './format.js';
import { lightSegments, sideCorners } from './geometry.js';

// Flat, distinguishable, and nothing like the game's palette on purpose: this
// is a diagram of a mine, and reading it as a screenshot would be a mistake.
const TILE_COLORS = [0x8a7f72, 0x6d7f8f, 0xa8873f, 0x6f7a63, 0x9aa0a6];

const PICKUP_COLOR = 0x6ad0e0;
const KEY_COLORS = [0x4a7dff, 0xf2d43a, 0xe04a3a];
const OBJECT_COLOR = 0xc06a4a;
const PORTAL_COLOR = 0x39d0a0;
const DOOR_COLOR = 0xe0913a;

// Groups rebuilt from the level on every build(), and cleared before it.
const LEVEL_GROUPS = ['walls', 'portals', 'doors', 'lights', 'reach', 'objects',
                      'pickups', 'start', 'exit', 'vertices', 'highlight'];

// Groups that do not come from the level and must survive a rebuild. Keeping
// this list explicit is what stops the next thing added to the scene from
// being re-created once per frame of a drag.
const PERMANENT_GROUPS = ['studio'];

const v3 = (p) => new THREE.Vector3(p[0], p[1], p[2]);

// The file stores quaternions as [w, x, y, z]; THREE.Quaternion is (x, y, z, w).
// This is the only place the two orders meet.
const quatFromLevel = (q) => new THREE.Quaternion(q[1], q[2], q[3], q[0]);

export class LevelView {
    constructor(models = null) {
        this.models = models;
        this.root = new THREE.Group();
        this.groups = {};
        for (const name of [...LEVEL_GROUPS, ...PERMANENT_GROUPS]) {
            this.groups[name] = new THREE.Group();
            this.groups[name].name = name;
            this.root.add(this.groups[name]);
        }
        this.buildStudioLighting();
        this.level = null;
        // Per wall triangle, which segment it belongs to, so a raycast hit can
        // be turned back into a segment index without searching.
        this.triangleSegment = [];
        this.disposables = [];
        this.wallMaterials = [];
        this.xray = false;
    }

    // Something for MeshLambertMaterial to respond to. Deliberately not the
    // level's lights: the mine is lit from a fixed pair of directions so that
    // geometry reads the same everywhere, including in the rooms the level
    // leaves black on purpose.
    //
    // Built ONCE, here, and never from build(). These do not depend on the
    // level, and build() runs on every frame of a gizmo drag - adding them
    // there stranded three more lights per frame, because dispose() clears the
    // groups and these used to hang off `root` where it could not see them.
    // The symptom was the whole scene washing out to white and the frame rate
    // collapsing as the drag went on. They live in a group for the same
    // reason: nothing of ours should be parented straight to `root`.
    buildStudioLighting() {
        const key = new THREE.DirectionalLight(0xffffff, 2.0);
        key.position.set(0.4, 1.0, 0.7);
        const fill = new THREE.DirectionalLight(0x88a0c0, 0.9);
        fill.position.set(-0.6, -0.4, -0.5);
        this.groups.studio.add(key, fill, new THREE.AmbientLight(0xffffff, 0.35));
    }

    setVisible(group, visible) {
        if (this.groups[group]) this.groups[group].visible = visible;
    }

    dispose() {
        for (const d of this.disposables) d.dispose();
        this.disposables.length = 0;
        this.wallMaterials.length = 0;
        for (const name of LEVEL_GROUPS) this.groups[name].clear();
    }

    track(resource) {
        this.disposables.push(resource);
        return resource;
    }

    // A mine seen from outside is a closed hull, so everything worth looking
    // at - portals, lights, pickups, the start - is behind a wall. X-ray drops
    // the walls to a shell you can see through without hiding them entirely,
    // which is the difference between a diagram of a level and a lump.
    setXray(on) {
        this.xray = on;
        for (const material of this.wallMaterials) {
            material.transparent = on;
            material.opacity = on ? 0.18 : 1.0;
            material.depthWrite = !on;
            material.needsUpdate = true;
        }
    }

    build(level) {
        this.dispose();
        this.level = level;
        this.buildWalls(level);
        this.buildPortals(level);
        this.buildLights(level);
        this.buildEntities(level);
        this.buildVertices(level);
        this.setXray(this.xray);
        return this;
    }

    // Every level vertex as a pickable point. Dragging one deforms every
    // segment that references it, which is the shared vertex array doing its
    // job: a portal cannot come apart because there is nothing to come apart.
    buildVertices(level) {
        const positions = [];
        for (const v of level.vertices) positions.push(v[0], v[1], v[2]);
        const geometry = this.track(new THREE.BufferGeometry());
        geometry.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
        const material = this.track(new THREE.PointsMaterial({
            color: 0xffe066, size: 7, sizeAttenuation: false, depthTest: false,
        }));
        const points = new THREE.Points(geometry, material);
        points.renderOrder = 998;
        points.name = 'vertices';
        this.groups.vertices.add(points);
        this.groups.vertices.visible = false;
    }

    // The eight corners of one segment, for framing the camera on it.
    segmentBounds(index) {
        const box = new THREE.Box3();
        for (const v of this.level.segments[index].verts) {
            box.expandByPoint(v3(this.level.vertices[v] ?? [0, 0, 0]));
        }
        return box;
    }

    // One BufferGeometry per texture index, so the mine draws in five meshes
    // and a level with a few thousand triangles still pans smoothly.
    buildWalls(level) {
        const byTile = new Map();
        this.triangleSegment = [];

        for (const wall of level.walls) {
            const tile = level.segments[wall.segment].sides[wall.side].texture;
            if (!byTile.has(tile)) byTile.set(tile, { positions: [], normals: [], faces: [] });
            const bucket = byTile.get(tile);
            for (const p of wall.p) {
                bucket.positions.push(p[0], p[1], p[2]);
                bucket.normals.push(wall.normal[0], wall.normal[1], wall.normal[2]);
            }
            // Per triangle, so a raycast hit becomes a segment AND a side
            // without searching. Extrude and the texture tools need the side.
            bucket.faces.push({ segment: wall.segment, side: wall.side });
        }

        for (const [tile, bucket] of byTile) {
            const geometry = this.track(new THREE.BufferGeometry());
            geometry.setAttribute('position', new THREE.Float32BufferAttribute(bucket.positions, 3));
            geometry.setAttribute('normal', new THREE.Float32BufferAttribute(bucket.normals, 3));

            // DoubleSide, and the back faces are darkened by the material's own
            // lighting rather than hidden: a mine is a space you are inside,
            // so its walls face away from anyone looking at it from outside,
            // and FrontSide would render the whole level invisible until the
            // camera was in it.
            const material = this.track(new THREE.MeshLambertMaterial({
                color: TILE_COLORS[tile] ?? 0xff00ff,
                side: THREE.DoubleSide,
                flatShading: true,
            }));

            this.wallMaterials.push(material);

            const mesh = new THREE.Mesh(geometry, material);
            mesh.name = `walls:${WALL_TILE_NAMES[tile] ?? tile}`;
            mesh.userData.faces = bucket.faces;
            this.groups.walls.add(mesh);
        }

        // Wall edges, drawn separately. A deformed cube reads as a box without
        // them and the deformation is the whole point of the format.
        const edges = [];
        for (const wall of level.walls) {
            for (let i = 0; i < 3; ++i) {
                const a = wall.p[i];
                const b = wall.p[(i + 1) % 3];
                edges.push(a[0], a[1], a[2], b[0], b[1], b[2]);
            }
        }
        const edgeGeometry = this.track(new THREE.BufferGeometry());
        edgeGeometry.setAttribute('position', new THREE.Float32BufferAttribute(edges, 3));
        const edgeMaterial = this.track(new THREE.LineBasicMaterial({
            color: 0x000000, transparent: true, opacity: 0.25,
        }));
        const lines = new THREE.LineSegments(edgeGeometry, edgeMaterial);
        lines.name = 'wall edges';
        this.groups.walls.add(lines);
    }

    // Each portal once, from the lower-numbered segment, so the two sides do
    // not z-fight. A portal carrying a door is drawn in the door colour.
    buildPortals(level) {
        const portal = [];
        const door = [];

        for (let si = 0; si < level.segments.length; ++si) {
            for (let side = 0; side < SIDE_COUNT; ++side) {
                const sd = level.segments[si].sides[side];
                if (sd.neighbour < 0 || sd.neighbour < si) continue;
                const c = sideCorners(level, si, side);
                const target = sd.door >= 0 ? door : portal;
                for (const [a, b, d] of [[0, 1, 2], [0, 2, 3]]) {
                    for (const k of [a, b, d]) target.push(c[k][0], c[k][1], c[k][2]);
                }
            }
        }

        for (const [name, data, color, opacity] of [
            ['portals', portal, PORTAL_COLOR, 0.16],
            ['doors', door, DOOR_COLOR, 0.45],
        ]) {
            if (data.length === 0) continue;
            const geometry = this.track(new THREE.BufferGeometry());
            geometry.setAttribute('position', new THREE.Float32BufferAttribute(data, 3));
            geometry.computeVertexNormals();
            const material = this.track(new THREE.MeshBasicMaterial({
                color, transparent: true, opacity, side: THREE.DoubleSide, depthWrite: false,
            }));
            this.groups[name].add(new THREE.Mesh(geometry, material));
        }
    }

    buildLights(level) {
        for (let i = 0; i < level.lights.length; ++i) {
            const light = level.lights[i];
            const color = new THREE.Color(light.color[0], light.color[1], light.color[2]);

            const bulb = new THREE.Mesh(
                this.track(new THREE.SphereGeometry(0.8, 12, 8)),
                this.track(new THREE.MeshBasicMaterial({ color })));
            bulb.position.copy(v3(light.position));
            bulb.userData.pick = { kind: 'light', index: i };
            this.groups.lights.add(bulb);

            // The radius is the flood fill's cutoff, not a falloff curve. It is
            // the number that decides which segments the light is considered
            // against at all, so it is worth seeing.
            const shell = new THREE.Mesh(
                this.track(new THREE.SphereGeometry(light.radius, 20, 14)),
                this.track(new THREE.MeshBasicMaterial({
                    color, wireframe: true, transparent: true, opacity: 0.1, depthWrite: false,
                })));
            shell.position.copy(v3(light.position));
            shell.userData.pick = { kind: 'light', index: i };
            this.groups.reach.add(shell);

            if (light.type === 'spot') {
                const direction = v3(light.direction).normalize();
                const helper = new THREE.ArrowHelper(direction, v3(light.position),
                                                     Math.min(light.radius, 12), color.getHex(), 2.5, 1.6);
                this.groups.lights.add(helper);
            }
        }
    }

    // The real mesh where the cache has it, a box where it does not. A level
    // naming a model that is not there is the author's problem to see, not a
    // reason for the editor to stop drawing - and the box makes it obvious.
    objectGeometry(object) {
        const model = this.models?.get(object.type);
        if (!model) return this.track(new THREE.BoxGeometry(3, 3, 3));
        const geometry = this.track(new THREE.BufferGeometry());
        geometry.setAttribute('position', new THREE.BufferAttribute(model.positions, 3));
        geometry.computeVertexNormals();
        return geometry;
    }

    buildEntities(level) {
        for (let i = 0; i < level.objects.length; ++i) {
            const object = level.objects[i];
            const mesh = new THREE.Mesh(this.objectGeometry(object),
                this.track(new THREE.MeshLambertMaterial({
                    color: OBJECT_COLOR, flatShading: true,
                })));
            mesh.position.copy(v3(object.position));
            mesh.quaternion.copy(quatFromLevel(object.orientation));
            mesh.scale.setScalar(object.scale);
            mesh.userData.pick = { kind: 'object', index: i };
            this.groups.objects.add(mesh);
        }

        for (let i = 0; i < level.pickups.length; ++i) {
            const pickup = level.pickups[i];
            const isKey = pickup.kind >= 6;
            const mesh = new THREE.Mesh(
                this.track(new THREE.OctahedronGeometry(1.4)),
                this.track(new THREE.MeshLambertMaterial({
                    color: isKey ? KEY_COLORS[pickup.kind - 6] : PICKUP_COLOR,
                })));
            mesh.position.copy(v3(pickup.position));
            mesh.userData.pick = { kind: 'pickup', index: i };
            this.groups.pickups.add(mesh);
        }

        const start = new THREE.Group();
        const body = new THREE.Mesh(
            this.track(new THREE.ConeGeometry(1.6, 5, 4)),
            this.track(new THREE.MeshLambertMaterial({ color: 0xffffff })));
        // A cone points +Y; the ship points -Z, which is the direction the
        // start orientation is written in.
        body.rotation.x = -Math.PI / 2;
        start.add(body);
        start.position.copy(v3(level.start.position));
        start.quaternion.copy(quatFromLevel(level.start.orientation));
        start.userData.pick = { kind: 'start', index: 0 };
        this.groups.start.add(start);

        if (level.exitTrigger) {
            const mesh = new THREE.Mesh(
                this.track(new THREE.SphereGeometry(level.exitTrigger.radius, 20, 14)),
                this.track(new THREE.MeshBasicMaterial({
                    color: 0x50ff90, wireframe: true, transparent: true, opacity: 0.4,
                })));
            mesh.position.copy(v3(level.exitTrigger.position));
            mesh.userData.pick = { kind: 'exit', index: 0 };
            this.groups.exit.add(mesh);
        }
    }

    // Outlines one segment's twelve edges. Used for the selection and for
    // showing a light's reach set, which is the thing that is hardest to
    // picture from the file and easiest to get wrong.
    highlightSegments(indices, color = 0xffe066) {
        this.groups.highlight.clear();
        if (!this.level || indices.length === 0) return;

        const points = [];
        for (const si of indices) {
            for (let side = 0; side < SIDE_COUNT; ++side) {
                const c = sideCorners(this.level, si, side);
                for (let i = 0; i < 4; ++i) {
                    const a = c[i];
                    const b = c[(i + 1) % 4];
                    points.push(a[0], a[1], a[2], b[0], b[1], b[2]);
                }
            }
        }

        const geometry = this.track(new THREE.BufferGeometry());
        geometry.setAttribute('position', new THREE.Float32BufferAttribute(points, 3));
        const material = this.track(new THREE.LineBasicMaterial({ color, depthTest: false }));
        const lines = new THREE.LineSegments(geometry, material);
        lines.renderOrder = 999;
        this.groups.highlight.add(lines);
    }

    // Fills one side, so "this is the wall you are about to extrude" is not a
    // guess. Drawn on top of everything, because the point is to see it even
    // when it is behind another segment.
    highlightSide(si, side, color = 0x6ad0e0) {
        this.groups.highlight.clear();
        if (!this.level || si < 0) return;
        const c = sideCorners(this.level, si, side);
        const positions = [];
        for (const [a, b, d] of [[0, 1, 2], [0, 2, 3]]) {
            for (const k of [a, b, d]) positions.push(c[k][0], c[k][1], c[k][2]);
        }
        const geometry = this.track(new THREE.BufferGeometry());
        geometry.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
        const material = this.track(new THREE.MeshBasicMaterial({
            color, transparent: true, opacity: 0.55, side: THREE.DoubleSide, depthTest: false,
        }));
        const mesh = new THREE.Mesh(geometry, material);
        mesh.renderOrder = 999;
        this.groups.highlight.add(mesh);
    }

    highlightVertex(index, color = 0xff8844) {
        this.groups.highlight.clear();
        if (!this.level || index < 0) return;
        const mesh = new THREE.Mesh(
            this.track(new THREE.SphereGeometry(1.1, 12, 8)),
            this.track(new THREE.MeshBasicMaterial({ color, depthTest: false })));
        mesh.position.copy(v3(this.level.vertices[index]));
        mesh.renderOrder = 999;
        this.groups.highlight.add(mesh);
    }

    lightReach(index) {
        const light = this.level?.lights[index];
        return light ? lightSegments(this.level, light) : [];
    }

    // Which wall a click landed on: raycast the wall meshes and read the
    // per-triangle record built with them. Returns { segment, side } or null.
    pickFace(raycaster) {
        for (const hit of raycaster.intersectObjects(this.groups.walls.children, false)) {
            const faces = hit.object.userData.faces;
            if (faces && hit.faceIndex !== undefined && faces[hit.faceIndex]) {
                return { ...faces[hit.faceIndex] };
            }
        }
        return null;
    }

    pickSegment(raycaster) {
        return this.pickFace(raycaster)?.segment ?? -1;
    }

    // Nearest level vertex under the cursor. The threshold is in world units
    // and set by the caller from the camera distance, so a vertex stays
    // clickable whether the view is on one segment or the whole mine.
    pickVertex(raycaster, threshold) {
        const points = this.groups.vertices.children[0];
        if (!points) return -1;
        const previous = raycaster.params.Points.threshold;
        raycaster.params.Points.threshold = threshold;
        const hits = raycaster.intersectObject(points, false);
        raycaster.params.Points.threshold = previous;
        return hits.length > 0 ? hits[0].index : -1;
    }

    pickEntity(raycaster) {
        const candidates = [...this.groups.lights.children, ...this.groups.objects.children,
                            ...this.groups.pickups.children, ...this.groups.start.children,
                            ...this.groups.exit.children];
        for (const hit of raycaster.intersectObjects(candidates, true)) {
            let node = hit.object;
            while (node && !node.userData.pick) node = node.parent;
            if (node) return node.userData.pick;
        }
        return null;
    }

    bounds() {
        const box = new THREE.Box3();
        if (this.level) {
            for (const v of this.level.vertices) box.expandByPoint(v3(v));
        }
        return box;
    }
}

export { SIDE_NAMES, quatFromLevel, v3 };
