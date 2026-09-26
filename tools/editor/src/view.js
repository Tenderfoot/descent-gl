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

import { BLOCK_TYPE, SIDE_COUNT, SIDE_NAMES, WALL_TILE_NAMES } from './format.js';
import { lampPosition } from './entities.js';
import { blockTriangles, lightSegments, portalFaces, portalFacing, sideCorners } from './geometry.js';

// Flat, distinguishable, and nothing like the game's palette on purpose: this
// is a diagram of a mine, and reading it as a screenshot would be a mistake.
const TILE_COLORS = [0x8a7f72, 0x6d7f8f, 0xa8873f, 0x6f7a63, 0x9aa0a6];

// The set is a manifest now, so there is no last tile to author a colour for.
// Past the authored ones the hue steps by the golden angle, which keeps
// neighbouring indices far apart in hue however many there are, at the same
// muted saturation as the list above.
function tileColor(tile) {
    if (TILE_COLORS[tile] !== undefined) return TILE_COLORS[tile];
    const hue = ((tile - TILE_COLORS.length) * 137.508) % 360;
    return new THREE.Color().setHSL(hue / 360, 0.22, 0.52).getHex();
}

const PICKUP_COLOR = 0x6ad0e0;
const KEY_COLORS = [0x4a7dff, 0xf2d43a, 0xe04a3a];
const OBJECT_COLOR = 0xc06a4a;
const PORTAL_COLOR = 0x39d0a0;
const DOOR_COLOR = 0xe0913a;
const GRATE_COLOR = 0x8fb8d8;

// Groups rebuilt from the level on every build(), and cleared before it.
const LEVEL_GROUPS = ['walls', 'portals', 'doors', 'grates', 'lights', 'reach', 'objects',
                      'pickups', 'props', 'start', 'exit', 'vertices', 'highlight'];

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
        this.textured = false;
        // Tile images by layer index. Not in `disposables`: they do not depend
        // on the level, and a drag rebuilds every frame.
        this.tileTextures = new Map();
        this.textureLoader = new THREE.TextureLoader();
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

    // The tile images on the walls, lit by the studio pair rather than the
    // level's lights. It is still not the game - no shadows, no falloff, and
    // a room the level leaves black is not black here - but it shows which
    // art a side carries and how its UVs lie across it, which a flat colour
    // per tile cannot.
    setTextured(on) {
        this.textured = on;
        for (const material of this.wallMaterials) this.applyTileLook(material);
    }

    // The tile's image, loaded once and shared by every build. The loader
    // flips rows the way wall_tiles.cpp does, so a tile is the same way up
    // here as on the wall. A tile that fails to load goes back to its colour.
    tileTexture(tile) {
        if (this.tileTextures.has(tile)) return this.tileTextures.get(tile);
        const name = WALL_TILE_NAMES[tile];
        if (name === undefined) return null;
        const texture = this.textureLoader.load(
            `/api/textures/${encodeURIComponent(name)}`,
            undefined,
            undefined,
            () => {
                console.warn(`could not load tile '${name}'; drawing it flat`);
                this.tileTextures.set(tile, null);
                texture.dispose();
                for (const material of this.wallMaterials) {
                    if (material.userData.tile === tile) this.applyTileLook(material);
                }
            });
        texture.wrapS = texture.wrapT = THREE.RepeatWrapping;
        texture.colorSpace = THREE.SRGBColorSpace;
        texture.anisotropy = 8;
        this.tileTextures.set(tile, texture);
        return texture;
    }

    applyTileLook(material) {
        const { tile, glow } = material.userData;
        const map = this.textured ? this.tileTexture(tile) : null;
        material.map = map;
        material.color.setHex(map ? 0xffffff : tileColor(tile));
        // An emitting side glows in its own art when there is art to glow in.
        material.emissiveMap = glow ? map : null;
        material.emissive.setHex(glow ? (map ? 0xffffff : tileColor(tile)) : 0x000000);
        material.needsUpdate = true;
    }

    build(level) {
        this.dispose();
        this.level = level;
        this.buildWalls(level);
        this.buildPortals(level);
        this.buildLights(level);
        this.buildProps(level);
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

    // One BufferGeometry per texture index, and a second per index for the sides
    // that emit, so the mine draws in a handful of meshes and a level with a few
    // thousand triangles still pans smoothly.
    buildWalls(level) {
        const byTile = new Map();
        this.triangleSegment = [];

        for (const wall of level.walls) {
            const sd = level.segments[wall.segment].sides[wall.side];
            const tile = sd.texture;
            // Emitting sides are their own bucket so the diagram can show them
            // glowing. Quantised to a flag rather than keyed on the value: this
            // is a picture of which walls emit, not of how much.
            const glow = sd.emissive > 0;
            const key = `${tile}:${glow ? 1 : 0}`;
            if (!byTile.has(key)) {
                byTile.set(key, { tile, glow, positions: [], normals: [], uvs: [], faces: [] });
            }
            const bucket = byTile.get(key);
            for (let k = 0; k < 3; ++k) {
                const p = wall.p[k];
                bucket.positions.push(p[0], p[1], p[2]);
                bucket.normals.push(wall.normal[0], wall.normal[1], wall.normal[2]);
                bucket.uvs.push(wall.uv[k][0], wall.uv[k][1]);
            }
            // Per triangle, so a raycast hit becomes a segment AND a side
            // without searching. Extrude and the texture tools need the side.
            bucket.faces.push({ segment: wall.segment, side: wall.side });
        }

        for (const { tile, glow, ...bucket } of byTile.values()) {
            const geometry = this.track(new THREE.BufferGeometry());
            geometry.setAttribute('position', new THREE.Float32BufferAttribute(bucket.positions, 3));
            geometry.setAttribute('normal', new THREE.Float32BufferAttribute(bucket.normals, 3));
            geometry.setAttribute('uv', new THREE.Float32BufferAttribute(bucket.uvs, 2));

            // DoubleSide, and the back faces are darkened by the material's own
            // lighting rather than hidden: a mine is a space you are inside,
            // so its walls face away from anyone looking at it from outside,
            // and FrontSide would render the whole level invisible until the
            // camera was in it.
            // An emitting side carries its own light in the game, so it
            // carries its own light here too - otherwise the one property the
            // author just set is the one the diagram does not show. Colour,
            // emissive and map are set by applyTileLook, which the Textures
            // toggle calls again.
            const material = this.track(new THREE.MeshLambertMaterial({
                emissiveIntensity: glow ? 0.9 : 0,
                side: THREE.DoubleSide,
                flatShading: true,
            }));
            material.userData = { tile, glow };
            this.applyTileLook(material);

            this.wallMaterials.push(material);

            const mesh = new THREE.Mesh(geometry, material);
            mesh.name = `walls:${WALL_TILE_NAMES[tile] ?? tile}${glow ? ':emissive' : ''}`;
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
    // not z-fight. A portal carrying a door is drawn in the door colour, and
    // one carrying a grate in the grate colour.
    //
    // The quads are pickable, and are the only way a portal can be: a portal
    // has no wall triangles, so a ray aimed at one used to pass straight
    // through the opening and hit the far wall of the next room. Each mesh
    // carries the same per-triangle record buildWalls() builds, so a hit
    // resolves to a side without searching.
    buildPortals(level) {
        const buckets = {
            portals: { positions: [], faces: [] },
            doors: { positions: [], faces: [] },
            grates: { positions: [], faces: [] },
        };

        for (const face of portalFaces(level)) {
            const bucket = face.door >= 0 ? buckets.doors
                         : face.grate >= 0 ? buckets.grates : buckets.portals;
            for (const [a, b, d] of [[0, 1, 2], [0, 2, 3]]) {
                for (const k of [a, b, d]) {
                    bucket.positions.push(face.corners[k][0], face.corners[k][1], face.corners[k][2]);
                }
                bucket.faces.push(face);
            }
        }

        for (const [name, color, opacity] of [
            ['portals', PORTAL_COLOR, 0.16],
            ['doors', DOOR_COLOR, 0.45],
            ['grates', GRATE_COLOR, 0.35],
        ]) {
            const bucket = buckets[name];
            if (bucket.positions.length === 0) continue;
            const geometry = this.track(new THREE.BufferGeometry());
            geometry.setAttribute('position', new THREE.Float32BufferAttribute(bucket.positions, 3));
            geometry.computeVertexNormals();
            const material = this.track(new THREE.MeshBasicMaterial({
                color, transparent: true, opacity, side: THREE.DoubleSide, depthWrite: false,
            }));
            const mesh = new THREE.Mesh(geometry, material);
            mesh.name = name;
            mesh.userData.faces = bucket.faces;
            this.groups[name].add(mesh);
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
            this.addReachShell(light.position, light.radius, color, { kind: 'light', index: i });

            if (light.type === 'spot') {
                const direction = v3(light.direction).normalize();
                const helper = new THREE.ArrowHelper(direction, v3(light.position),
                                                     Math.min(light.radius, 12), color.getHex(), 2.5, 1.6);
                this.groups.lights.add(helper);
            }

            // A light authored with a visible source. The game draws a
            // camera-facing emissive quad of this half-extent; a wire sphere
            // is the same size from every angle the editor can be turned to,
            // and leaves the bulb underneath it visible.
            if (light.fixtureSize > 0) {
                const fixture = new THREE.Mesh(
                    this.track(new THREE.IcosahedronGeometry(light.fixtureSize, 1)),
                    this.track(new THREE.MeshBasicMaterial({
                        color, wireframe: true, transparent: true, opacity: 0.55,
                    })));
                fixture.position.copy(v3(light.position));
                fixture.userData.pick = { kind: 'light', index: i };
                this.groups.lights.add(fixture);
            }
        }
    }

    // The two things that move once the level is running.
    //
    // Both are drawn at rest, and both draw the part that cannot be read off
    // the numbers: a floating light's reach, and a swinging lamp's whole arc.
    // The arc is the one that matters - a cord length and an amplitude in
    // radians say nothing about whether the lamp will sweep into a pillar, and
    // the swept curve says it at a glance.
    buildProps(level) {
        for (let i = 0; i < level.floatingLights.length; ++i) {
            const ball = level.floatingLights[i];
            const color = new THREE.Color(ball.color[0], ball.color[1], ball.color[2]);

            const mesh = new THREE.Mesh(
                this.track(new THREE.SphereGeometry(ball.radius, 16, 12)),
                this.track(new THREE.MeshBasicMaterial({ color })));
            mesh.position.copy(v3(ball.position));
            mesh.userData.pick = { kind: 'ball', index: i };
            this.groups.props.add(mesh);

            // A wire cage, so a ball reads as a lamp in a housing rather than
            // as a pickup, and so its collision radius is visible.
            const cage = new THREE.Mesh(
                this.track(new THREE.IcosahedronGeometry(ball.radius * 1.35, 1)),
                this.track(new THREE.MeshBasicMaterial({
                    color, wireframe: true, transparent: true, opacity: 0.55,
                })));
            cage.position.copy(v3(ball.position));
            cage.userData.pick = { kind: 'ball', index: i };
            this.groups.props.add(cage);

            this.addReachShell(ball.position, ball.lightRadius, color, { kind: 'ball', index: i });
        }

        for (let i = 0; i < level.swingingLights.length; ++i) {
            const lamp = level.swingingLights[i];
            const color = new THREE.Color(lamp.color[0], lamp.color[1], lamp.color[2]);
            const pick = { kind: 'lamp', index: i };
            const rest = lampPosition(lamp, 0);

            // The anchor is what the author drags, so it is what is easiest to
            // hit: a small solid marker at the ceiling.
            const anchor = new THREE.Mesh(
                this.track(new THREE.BoxGeometry(0.9, 0.5, 0.9)),
                this.track(new THREE.MeshBasicMaterial({ color: 0x8899aa })));
            anchor.position.copy(v3(lamp.position));
            anchor.userData.pick = pick;
            this.groups.props.add(anchor);

            const housing = new THREE.Mesh(
                this.track(new THREE.ConeGeometry(1.3, 1.8, 12)),
                this.track(new THREE.MeshBasicMaterial({ color })));
            housing.position.copy(v3(rest));
            housing.userData.pick = pick;
            this.groups.props.add(housing);

            // The cord at rest, and then the arc the lamp actually sweeps.
            this.addLine([lamp.position, rest], 0x8899aa, 0.9, pick);

            const arc = [];
            const steps = 24;
            for (let k = 0; k <= steps; ++k) {
                arc.push(lampPosition(lamp, -lamp.amplitude + (2 * lamp.amplitude * k) / steps));
            }
            this.addLine(arc, color.getHex(), 0.75, pick);
            // The two extremes of the cord, so the swept volume is readable
            // rather than just its lower edge.
            this.addLine([lamp.position, arc[0]], color.getHex(), 0.3, pick);
            this.addLine([lamp.position, arc[arc.length - 1]], color.getHex(), 0.3, pick);

            const direction = new THREE.Vector3().subVectors(v3(rest), v3(lamp.position)).normalize();
            this.groups.props.add(new THREE.ArrowHelper(
                direction, v3(rest), Math.min(lamp.lightRadius, 12), color.getHex(), 2.5, 1.6));

            this.addReachShell(rest, lamp.lightRadius, color, pick);
        }
    }

    // A light's radius is the flood fill's cutoff, not a falloff curve: it is
    // the number that decides which segments the light is considered against at
    // all. Shared by the static lights and both prop kinds.
    addReachShell(position, radius, color, pick) {
        const shell = new THREE.Mesh(
            this.track(new THREE.SphereGeometry(radius, 20, 14)),
            this.track(new THREE.MeshBasicMaterial({
                color, wireframe: true, transparent: true, opacity: 0.1, depthWrite: false,
            })));
        shell.position.copy(v3(position));
        if (pick) shell.userData.pick = pick;
        this.groups.reach.add(shell);
    }

    addLine(points, color, opacity, pick) {
        const geometry = this.track(new THREE.BufferGeometry());
        geometry.setFromPoints(points.map(v3));
        const material = this.track(new THREE.LineBasicMaterial({
            color, transparent: true, opacity,
        }));
        const line = new THREE.Line(geometry, material);
        if (pick) line.userData.pick = pick;
        this.groups.props.add(line);
        return line;
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

    // A block is drawn as what it is: its own size, with the game's UVs, in its
    // tile's colour or art. Its material joins the walls' so the texture and
    // x-ray toggles treat it as the piece of architecture it is.
    buildBlock(object, index) {
        const { positions, normals, uvs } = blockTriangles(object);
        const geometry = this.track(new THREE.BufferGeometry());
        geometry.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
        geometry.setAttribute('normal', new THREE.Float32BufferAttribute(normals, 3));
        geometry.setAttribute('uv', new THREE.Float32BufferAttribute(uvs, 2));
        const material = this.track(new THREE.MeshLambertMaterial({ color: tileColor(object.texture) }));
        material.userData = { tile: object.texture, glow: false };
        this.applyTileLook(material);
        this.wallMaterials.push(material);
        const mesh = new THREE.Mesh(geometry, material);
        mesh.position.copy(v3(object.position));
        mesh.quaternion.copy(quatFromLevel(object.orientation));
        mesh.userData.pick = { kind: 'object', index };
        this.groups.objects.add(mesh);
    }

    buildEntities(level) {
        for (let i = 0; i < level.objects.length; ++i) {
            const object = level.objects[i];
            if (object.type === BLOCK_TYPE) {
                this.buildBlock(object, i);
                continue;
            }
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
        this.highlightVertices(index < 0 ? [] : [index], color);
    }

    // Every vertex in the selection, with the last one in the list drawn
    // brighter.
    //
    // That last one is the primary - the one the author just clicked, the one
    // the panel describes, the one a single-vertex tool would act on - and a
    // group of eight identical blobs loses it. One geometry and two materials
    // for the whole group rather than a pair per vertex, because a segment's
    // eight corners are selected in one action and this runs on every rebuild
    // of a drag.
    highlightVertices(indices, color = 0xff8844, primaryColor = 0xfff0a0) {
        this.groups.highlight.clear();
        if (!this.level || indices.length === 0) return;

        const geometry = this.track(new THREE.SphereGeometry(1.1, 12, 8));
        const material = this.track(new THREE.MeshBasicMaterial({ color, depthTest: false }));
        const primary = this.track(new THREE.MeshBasicMaterial({
            color: primaryColor, depthTest: false }));

        indices.forEach((index, i) => {
            const position = this.level.vertices[index];
            if (!position) return;
            const mesh = new THREE.Mesh(geometry,
                                        i === indices.length - 1 ? primary : material);
            mesh.position.copy(v3(position));
            mesh.renderOrder = 999;
            this.groups.highlight.add(mesh);
        });
    }

    lightReach(index) {
        const light = this.level?.lights[index];
        return light ? lightSegments(this.level, light) : [];
    }

    // Which side a click landed on: raycast the meshes and read the
    // per-triangle record built with them. Returns { segment, side } or null.
    //
    // Portals are included only where a face is what the click is for - side
    // and entity mode - because in segment mode a click through a doorway
    // means the room beyond it, which is how double-click flies from room to
    // room. A hidden group is not pickable: unticking Portals in Show is how
    // an author reaches a wall that a portal quad is in front of.
    pickFace(raycaster, { portals = false } = {}) {
        const meshes = [...this.groups.walls.children];
        if (portals) {
            for (const name of ['portals', 'doors', 'grates']) {
                if (this.groups[name].visible) meshes.push(...this.groups[name].children);
            }
        }

        // intersectObjects sorts by distance across every mesh, so the first
        // hit carrying a record is the nearest side along the ray.
        for (const hit of raycaster.intersectObjects(meshes, false)) {
            const faces = hit.object.userData.faces;
            const face = hit.faceIndex !== undefined ? faces?.[hit.faceIndex] : undefined;
            if (!face) continue;
            // A portal is a side of two segments; a wall is a side of one.
            // geometry.js speaks plain arrays and knows nothing about three.js,
            // so the ray direction is unpacked rather than handed over: a
            // Vector3 indexed as an array is three undefineds and a NaN, and a
            // NaN comparison silently picks the wrong end of every portal.
            const { x, y, z } = raycaster.ray.direction;
            return face.neighbour === undefined
                ? { segment: face.segment, side: face.side }
                : portalFacing(face, [x, y, z]);
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

    // What a click on a gizmo landed on, or null. Nearest first.
    //
    // `props` is in here so the two moving kinds can be clicked at all: their
    // meshes, cords and swept arcs already carry a pick record, but the group
    // they live in was not in this list, so a floating ball and a swinging lamp
    // were selectable only from the panel - no use to a shift+click that is
    // meant to gather a group.
    //
    // Hidden groups are skipped, the way pickFace skips a hidden portal group.
    // That matters more now: a lamp draws its whole swept arc and a light can
    // draw its reach, so unticking the layer is how an author reaches the wall
    // behind one.
    pickEntity(raycaster) {
        const candidates = [];
        for (const name of ['lights', 'objects', 'pickups', 'props', 'start', 'exit']) {
            if (this.groups[name].visible) candidates.push(...this.groups[name].children);
        }
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
