// The editor application: load a level, show it, edit its structure, save it
// back, and ask the game what it thinks of the result.
//
// Every edit goes through one path - `edit()` below - which records undo,
// applies the operation, revalidates and redraws. Nothing calls into ops.js
// directly, so there is no way to change a level and forget to re-check it.

import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { TransformControls } from 'three/addons/controls/TransformControls.js';

import {
    addDoor, addFloatingLight, addGrate, addLight, addObject, addPickup, addSwingingLight,
    availableKeys, ceilingAnchor, duplicateEntity, ENTITY_LISTS, entityFacing, entityList,
    hasReactor, moveEntities, moveEntity, removeEntity, segmentCentre, setExit, setGrateTexture,
    setGrateUvMode,
} from './entities.js';
import {
    BLOCK_TYPE, DIFFICULTY_NAMES, KEY_NAMES, LIGHT_FIXTURE_SIZE, PICKUP_KEY_FIRST, PICKUP_NAMES,
    SIDE_NAMES, SIDE_VERTICES, UV_MODES, WALL_TILE_NAMES, setWallTileNames,
} from './format.js';
import { ModelCache } from './obj.js';
import { checkLevelName, checkName, newLevel } from './create.js';
import { GENERATED_MARK, generateMine } from './generate.js';
import { History } from './history.js';
import { emitJson } from './json_emit.js';
import { parseLevel, resolveSegments, serializeLevel } from './level.js';
import { add, distance, scale, sideCorners } from './geometry.js';
import {
    deleteSegment, extrudeSide, joinSides, moveVertex, moveVertices, segmentDepth, setEmissive,
    nearestSnapTarget, setLevelUvMode, setRotation, setTexture, setUvMode, snapVertex, splitPortal,
} from './ops.js';
import {
    centroid, conjugateQuat, edgeFrame, frameFromAxes, fromLevelQuat, groupTransform,
    IDENTITY_QUAT, multiplyQuat, rotateVector, spreadFrame, transformPoints,
} from './transform.js';
import { EDITOR_ONLY_RULES, validateLevel } from './validate.js';
import { LevelView } from './view.js';

const $ = (id) => document.getElementById(id);

const LAYERS = [
    ['walls', 'Walls', true],
    ['textures', 'Textures', true],
    ['xray', 'X-ray', false],
    ['portals', 'Portals', true],
    ['doors', 'Doors', true],
    ['grates', 'Grates', true],
    ['lights', 'Lights', true],
    ['reach', 'Light radius', false],
    ['objects', 'Objects', true],
    ['pickups', 'Pickups', true],
    ['props', 'Moving lights', true],
    ['start', 'Start', true],
    ['exit', 'Exit', true],
];

const MODES = [['segment', 'Segment'], ['side', 'Side'], ['vertex', 'Vertex'],
               ['entity', 'Entity']];

// What the gizmo does. Translate is the only one that means anything for a
// single vertex - rotating a point about itself is a no-op - so the other two
// are here for groups, and for the one single case that does have a facing of
// its own: a piece of scenery.
const GIZMO_MODES = [['translate', 'Move', 'W'], ['rotate', 'Turn', 'E'],
                     ['scale', 'Scale', 'R']];

const models = new ModelCache();

const app = {
    level: null,
    name: null,
    validation: null,
    selection: null,
    mode: 'segment',
    joinFrom: null,
    lastEditDetail: null,
    lastSegment: null,
    models: [],
    history: new History(),
    // Which way the gizmo's handles point: 'world' axes, or the selection's
    // own - a wall's across, up and normal. Q flips it.
    gizmoSpace: 'world',
};

// --- scene ------------------------------------------------------------------

const viewport = $('viewport');
const panelResizer = $('panel-resizer');
const PANEL_MIN = 280;
const PANEL_MAX = 720;
const VIEWPORT_MIN = 240;

function setPanelWidth(width, remember = true) {
    const available = Math.max(PANEL_MIN, window.innerWidth - VIEWPORT_MIN - panelResizer.offsetWidth);
    const next = Math.round(Math.min(Math.max(width, PANEL_MIN), PANEL_MAX, available));
    document.documentElement.style.setProperty('--panel-width', `${next}px`);
    panelResizer.setAttribute('aria-valuemax', String(Math.min(PANEL_MAX, available)));
    panelResizer.setAttribute('aria-valuenow', String(next));
    if (remember) localStorage.setItem('descent-editor-panel-width', String(next));
}

const savedPanelWidth = Number(localStorage.getItem('descent-editor-panel-width'));
setPanelWidth(Number.isFinite(savedPanelWidth) && savedPanelWidth > 0 ? savedPanelWidth : 340, false);

let resizingPanel = false;
panelResizer.addEventListener('pointerdown', (e) => {
    resizingPanel = true;
    panelResizer.classList.add('dragging');
    panelResizer.setPointerCapture(e.pointerId);
    e.preventDefault();
});
panelResizer.addEventListener('pointermove', (e) => {
    if (resizingPanel) setPanelWidth(window.innerWidth - e.clientX);
});
const finishPanelResize = (e) => {
    resizingPanel = false;
    panelResizer.classList.remove('dragging');
    if (panelResizer.hasPointerCapture(e.pointerId)) panelResizer.releasePointerCapture(e.pointerId);
};
panelResizer.addEventListener('pointerup', finishPanelResize);
panelResizer.addEventListener('pointercancel', finishPanelResize);
panelResizer.addEventListener('keydown', (e) => {
    if (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight') return;
    const width = Number.parseFloat(getComputedStyle(document.documentElement)
        .getPropertyValue('--panel-width')) || 340;
    setPanelWidth(width + (e.key === 'ArrowLeft' ? 20 : -20));
    e.preventDefault();
});
window.addEventListener('resize', () => {
    const width = Number(panelResizer.getAttribute('aria-valuenow')) || 340;
    setPanelWidth(width, false);
});

const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
viewport.appendChild(renderer.domElement);

const hud = document.createElement('div');
hud.id = 'hud';
hud.textContent = 'drag to orbit · click to select · shift+click to add · double-click to fly in '
    + '· 1/2/3/4 mode · W/E/R gizmo · Q local/world · T top · F frame';
viewport.appendChild(hud);

const scene = new THREE.Scene();
scene.background = new THREE.Color(0x14161a);

// Two real cameras rather than one with its type flags flipped: the renderer,
// the raycaster and the gizmo all branch on isOrthographicCamera, and an
// object that claims to be both is a bug waiting for whichever of them checks
// last.
const perspective = new THREE.PerspectiveCamera(60, 1, 0.1, 5000);
const orthographic = new THREE.OrthographicCamera(-1, 1, 1, -1, 0.1, 10000);
let camera = perspective;

const controls = new OrbitControls(camera, renderer.domElement);
controls.enableDamping = true;

// The automap's view: straight down, orthographic, so the mine reads as a plan
// rather than as a lump in perspective. It is the view that makes "this
// corridor doubles back over that room" obvious, which is why the game's own
// automap uses it.
let topDown = false;

function setTopDown(on) {
    topDown = on;
    $('top-view').setAttribute('aria-pressed', String(on));

    camera = on ? orthographic : perspective;
    controls.object = camera;
    gizmo.camera = camera;

    const box = view.bounds();
    if (box.isEmpty()) return;
    const center = box.getCenter(new THREE.Vector3());
    const size = box.getSize(new THREE.Vector3());

    if (on) {
        fitOrthographic(Math.max(size.x, size.z) * 0.6 + 1);
        orthographic.near = 0.1;
        orthographic.far = size.y * 4 + 4000;
        // Looking straight down. The hair of Z keeps the up vector from being
        // parallel to the view direction, which leaves the matrix degenerate.
        orthographic.position.set(center.x, center.y + size.y + 500, center.z + 0.001);
        controls.target.copy(center);
        controls.enableRotate = false;   // a plan you can tumble is perspective again
        orthographic.updateProjectionMatrix();
    } else {
        controls.enableRotate = true;
        frameLevel();
    }
    controls.update();
}

function fitOrthographic(span) {
    const aspect = viewport.clientWidth / Math.max(viewport.clientHeight, 1);
    orthographic.left = -span * aspect;
    orthographic.right = span * aspect;
    orthographic.top = span;
    orthographic.bottom = -span;
    orthographic.updateProjectionMatrix();
}

const view = new LevelView(models);
scene.add(view.root);

// A stand-in for the vertex being dragged. It lives outside view.root so that
// rebuilding the level mid-drag does not pull the gizmo out from under the
// pointer.
const handle = new THREE.Object3D();
scene.add(handle);

const gizmo = new TransformControls(camera, renderer.domElement);
gizmo.addEventListener('dragging-changed', (e) => { controls.enabled = !e.value; });
scene.add(gizmo.getHelper ? gizmo.getHelper() : gizmo);
gizmo.detach();

function resize() {
    const { clientWidth: w, clientHeight: h } = viewport;
    renderer.setSize(w, h, false);
    perspective.aspect = w / Math.max(h, 1);
    perspective.updateProjectionMatrix();
    if (topDown) fitOrthographic(orthographic.top);
}
new ResizeObserver(resize).observe(viewport);
resize();

(function frame() {
    requestAnimationFrame(frame);
    controls.update();
    renderer.render(scene, camera);
}());

function frameSegment(index) {
    const box = view.segmentBounds(index);
    const center = box.getCenter(new THREE.Vector3());
    const radius = Math.max(box.getSize(new THREE.Vector3()).length() * 0.5, 1);
    controls.target.copy(center);
    if (topDown) {
        fitOrthographic(radius * 1.6);
        orthographic.position.set(center.x, orthographic.position.y, center.z + 0.001);
        controls.update();
        return;
    }
    perspective.position.copy(center).add(new THREE.Vector3(radius, radius * 0.8, radius * 1.2));
    perspective.near = Math.max(radius / 500, 0.05);
    perspective.far = Math.max(perspective.far, radius * 200);
    perspective.updateProjectionMatrix();
    controls.update();
}

function frameLevel() {
    const box = view.bounds();
    if (box.isEmpty()) return;
    if (topDown) { setTopDown(true); return; }
    const center = box.getCenter(new THREE.Vector3());
    const radius = box.getSize(new THREE.Vector3()).length() * 0.5;
    controls.target.copy(center);
    perspective.position.copy(center).add(new THREE.Vector3(radius * 0.9, radius * 0.8, radius * 1.1));
    perspective.near = Math.max(radius / 1000, 0.05);
    perspective.far = radius * 12;
    perspective.updateProjectionMatrix();
    controls.update();
}

// --- status -----------------------------------------------------------------

let statusTimer = 0;
function status(text, kind = '') {
    const el = $('status');
    el.textContent = text;
    el.className = `status ${kind}`;
    clearTimeout(statusTimer);
    if (kind === 'ok') statusTimer = setTimeout(() => { renderDirty(); }, 4000);
}

function renderDirty() {
    const el = $('status');
    if (!app.level) return;
    if (app.history.isDirty(app.level)) {
        el.textContent = 'unsaved changes';
        el.className = 'status dirty';
    } else {
        el.textContent = '';
        el.className = 'status';
    }
}

// --- the one path every edit takes ------------------------------------------

// Records undo, runs the operation, revalidates, redraws. An operation that
// throws leaves the level untouched and says why, because ops.js checks its
// preconditions before it changes anything.
function edit(label, operation) {
    if (!app.level) return;
    app.history.record(app.level, label);
    try {
        operation(app.level);
    } catch (e) {
        app.history.past.pop();
        status(e.message, 'error');
        return;
    }
    resolveSegments(app.level);
    refresh();
    status(app.lastEditDetail ? `${label} ${app.lastEditDetail}` : label, 'ok');
    app.lastEditDetail = null;
}

// Rebuilds everything downstream of the level: the checks, the scene, the
// panel. Deliberately unconditional - a partial refresh is how an editor ends
// up showing a stale wall.
function refresh() {
    app.validation = validateLevel(app.level);
    view.build(app.level);
    applyLayers();
    renderStats();
    renderFindings();
    renderTools();
    renderSelection();
    renderHistoryButtons();
    syncGizmo();
}

function replaceLevel(next) {
    if (!next) return;
    app.level = next;
    app.selection = null;
    app.joinFrom = null;
    gizmo.detach();
    refresh();
}

// --- loading and saving -----------------------------------------------------

// The tile names the game would load, so the picker offers what the manifest
// has rather than what this file was last told about. A set that will not load
// is not worth stopping the editor over - the names in format.js still let
// every existing level be edited - so this reports and carries on.
async function loadTileNames() {
    try {
        const response = await fetch('/api/textures');
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        const { tiles } = await response.json();
        setWallTileNames(tiles);
    } catch (e) {
        console.warn(`could not read assets/textures/tiles.json (${e.message}); using the built-in tile names`);
    }
}

async function listLevels() {
    const response = await fetch('/api/levels');
    if (!response.ok) throw new Error(`could not list levels (${response.status})`);
    return response.json();
}

async function loadLevel(name) {
    const response = await fetch(`/api/levels/${encodeURIComponent(name)}`);
    if (!response.ok) throw new Error(`could not read ${name} (${response.status})`);
    const text = await response.text();

    app.name = name;
    app.level = parseLevel(text, `assets/levels/${name}.json`);
    // Fetch every mesh the level names before the first draw, so objects come
    // up as themselves rather than as boxes that turn into robots a moment
    // later.
    await models.loadAll(app.level.objects.map((o) => o.type).filter((t) => t !== BLOCK_TYPE));
    app.selection = null;
    app.joinFrom = null;
    gizmo.detach();
    app.history.reset(app.level);

    refresh();
    frameLevel();
    $('verify-section').hidden = true;
    status(`loaded ${name}`, 'ok');
}

async function saveLevel() {
    if (!app.level) return;
    const errors = app.validation.findings.filter((f) => f.severity === 'error');
    if (errors.length > 0) {
        // Writing a level the game will abort on makes every later game start
        // fatal, editor or not. Saying so is more useful than saving it.
        throw new Error(`${errors.length} error(s) to fix first: ${errors[0].message}`);
    }

    const body = emitJson(serializeLevel(app.level));
    const response = await fetch(`/api/levels/${encodeURIComponent(app.name)}`, {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body,
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error ?? `save failed (${response.status})`);
    app.history.markSaved(app.level);
    status(`wrote ${result.wrote} (${result.bytes} bytes)`, 'ok');
}

async function verify() {
    $('verify').disabled = true;
    status('running the game’s checks…');
    try {
        const response = await fetch('/api/verify', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ level: app.name }),
        });
        const result = await response.json();
        if (!response.ok) throw new Error(result.error ?? `verify failed (${response.status})`);

        $('verify-section').hidden = false;
        renderChecklist(result.results);
        $('verify-output').textContent = result.results
            .map((r) => `$ descent ${r.argv.join(' ')}\n[exit ${r.exit}]\n${r.output.trim()}`)
            .join('\n\n');

        // The only hard failure is a level the game could not load. Everything
        // else is an assertion that did not hold, and some of those say the
        // level is too small to exercise the system under test rather than
        // that it is wrong.
        const broken = result.results.filter((r) => r.aborted);
        const objected = result.results.filter((r) => !r.aborted && !r.advisory && r.notes.length > 0);

        const summary = broken.length > 0
            ? `the game cannot load this level (${broken.map((r) => r.check).join(', ')})`
            : objected.length > 0
                ? `loads cleanly; ${objected.reduce((n, r) => n + r.notes.length, 0)} note(s) below`
                : 'all checks passed';
        status(summary, broken.length > 0 ? 'error' : 'ok');
    } finally {
        $('verify').disabled = false;
    }
}

// One row per check. What an author needs from these is the difference between
// "the game cannot load this" and "the game loaded it and had opinions", which
// a bare exit code does not give.
function renderChecklist(results) {
    const list = $('verify-list');
    list.innerHTML = '';

    for (const result of results) {
        const el = document.createElement('div');
        let verdict;
        let why = '';

        if (result.aborted) {
            el.className = 'check fail';
            verdict = 'cannot load';
            why = 'The game aborted reading this level. Nothing else matters until this is fixed.';
        } else if (result.advisory) {
            el.className = 'check note';
            verdict = 'advisory';
            why = `--${result.check}-test is ${result.writtenFor}'s regression test, not a check ` +
                  'of this level: it asserts that level\'s own shape. Shown, not counted.';
        } else if (result.notes.length > 0) {
            el.className = 'check note';
            verdict = `${result.notes.length} note(s)`;
            why = 'The level loaded. A check can object because the level is too small to ' +
                  'exercise what it tests — no dark segment, culling with nothing to cull — ' +
                  'rather than because it is wrong.';
        } else {
            el.className = 'check pass';
            verdict = 'pass';
        }

        const head = document.createElement('div');
        head.className = 'head';
        head.innerHTML = `<span>descent ${escapeHtml(result.argv[0])}</span>` +
                         `<span class="verdict">${verdict}</span>`;
        el.appendChild(head);

        if (result.notes.length > 0) {
            const ul = document.createElement('ul');
            for (const note of result.notes.slice(0, 8)) {
                const li = document.createElement('li');
                li.textContent = note;
                ul.appendChild(li);
            }
            if (result.notes.length > 8) {
                const li = document.createElement('li');
                li.textContent = `…and ${result.notes.length - 8} more, in the full output`;
                ul.appendChild(li);
            }
            el.appendChild(ul);
        }

        if (why) {
            const p = document.createElement('div');
            p.className = 'why';
            p.textContent = why;
            el.appendChild(p);
        }

        list.appendChild(el);
    }
}

// --- tools ------------------------------------------------------------------

function setMode(mode) {
    app.mode = mode;
    app.joinFrom = null;
    gizmo.detach();
    app.selection = null;
    view.groups.vertices.visible = mode === 'vertex';
    for (const button of $('modes').querySelectorAll('button')) {
        button.setAttribute('aria-pressed', String(button.dataset.mode === mode));
    }
    renderTools();
    renderSelection();
}

// A picker for a "uvMode". `inherits` names what null falls back to and adds a
// row for it; without it the choice is only side or world, and side is null
// so that the default writes nothing to the file.
function uvModeField(container, label, title, current, inherits, onpick) {
    const wrap = document.createElement('label');
    wrap.className = 'field';
    wrap.title = title;
    wrap.append(label);
    const select = document.createElement('select');
    const choices = inherits ? [[null, `level (${inherits})`], ...UV_MODES.map((m) => [m, m])]
                             : UV_MODES.map((m) => [m === 'side' ? null : m, m]);
    for (const [mode, text] of choices) {
        const option = document.createElement('option');
        option.value = mode ?? '';
        option.textContent = text;
        select.appendChild(option);
    }
    select.value = inherits ? (current ?? '') : (current === 'world' ? 'world' : '');
    select.onchange = () => onpick(select.value === '' ? null : select.value);
    wrap.appendChild(select);
    container.appendChild(wrap);
}

const UV_MODE_TITLE = 'How the tile is laid when the side has no authored uv. side fits it to ' +
                      'this side alone; world lays one grid through the mine, so neighbouring ' +
                      'sides line up - for grids and grates, not for tiles with writing on them.';

function renderTools() {
    const container = $('tools');
    container.innerHTML = '';
    const selection = app.selection;

    const button = (label, title, onclick) => {
        const el = document.createElement('button');
        el.textContent = label;
        el.title = title;
        el.onclick = onclick;
        container.appendChild(el);
        return el;
    };
    const hint = (text) => {
        const el = document.createElement('div');
        el.className = 'hint';
        el.textContent = text;
        container.appendChild(el);
    };

    if (app.mode === 'segment') {
        if (selection?.kind !== 'segment') return hint('Click a segment.');
        // The whole room as a selection. Shift+clicking eight corners one at a
        // time works and is nobody's idea of a good afternoon.
        button('Select its 8 vertices',
               'Switches to vertex mode with this segment\u2019s corners selected, so the ' +
               'gizmo moves, turns or scales the whole room',
               () => selectVertices(app.level.segments[selection.index].verts,
                                    `selected segment ${selection.index}'s vertices`));
        button('Delete segment',
               'Seals the neighbours, reindexes everything, and takes the lights, pickups and ' +
               'objects inside it with it',
               () => edit(`delete segment ${selection.index}`, (level) => {
                   const gone = deleteSegment(level, selection.index);
                   app.selection = null;
                   const also = Object.entries(gone)
                       .filter(([, n]) => n > 0)
                       .map(([what, n]) => `${n} ${what}`)
                       .join(', ');
                   if (also) app.lastEditDetail = `and ${also}`;
               }));
        return;
    }

    if (app.mode === 'side') {
        if (selection?.kind !== 'side') {
            return hint(app.joinFrom
                ? `Join from segment ${app.joinFrom.segment} ${SIDE_NAMES[app.joinFrom.side]} — ` +
                  'now click the wall to join it to.'
                : 'Click a wall or a portal.');
        }
        const { segment, side } = selection;
        const sd = app.level.segments[segment].sides[side];

        // Offered for a portal too: the four vertices a portal shares are the
        // ones both segments deform through, and moving them is how a doorway
        // gets widened without breaking the share.
        button('Select its 4 vertices',
               'Switches to vertex mode with this side\u2019s corners selected, so the gizmo ' +
               'moves, turns or scales the whole wall',
               () => selectVertices(SIDE_VERTICES[side].map((c) => app.level.segments[segment].verts[c]),
                                    `selected segment ${segment} ${SIDE_NAMES[side]} vertices`));

        if (sd.neighbour >= 0) {
            hint(`portal → segment ${sd.neighbour}`);
            button('Split portal', 'Turns the portal back into two walls',
                   () => edit(`split portal ${segment} ${SIDE_NAMES[side]}`,
                              (level) => splitPortal(level, segment, side)));
            return;
        }

        button('Extrude', 'Grows a new segment out of this wall, sharing its four vertices',
               () => edit(`extrude ${segment} ${SIDE_NAMES[side]}`, (level) => {
                   const created = extrudeSide(level, segment, side);
                   app.selection = { kind: 'segment', index: created };
                   app.mode = 'segment';
                   setModeButtons();
               }));

        const armed = app.joinFrom;
        if (armed && armed.segment === segment && armed.side === side) {
            // The armed wall is the one selected. Saying "Join from here" again
            // would look like nothing had happened.
            hint('Join armed — now click the wall to join this to.');
            button('Cancel join', '', () => { app.joinFrom = null; renderTools(); });
        } else if (armed) {
            const from = armed;
            button(`Join to ${from.segment} ${SIDE_NAMES[from.side]}`,
                   'Makes the two walls one portal, sharing their four vertex indices',
                   () => edit(`join ${from.segment}/${SIDE_NAMES[from.side]} to ` +
                              `${segment}/${SIDE_NAMES[side]}`, (level) => {
                       joinSides(level, from.segment, from.side, segment, side);
                       app.joinFrom = null;
                   }));
            button('Cancel join', '', () => { app.joinFrom = null; renderTools(); });
        } else {
            button('Join from here…', 'Then click the wall to join it to',
                   () => { app.joinFrom = { segment, side }; renderTools(); });
        }

        const textures = document.createElement('div');
        textures.className = 'textures';
        WALL_TILE_NAMES.forEach((name, tile) => {
            const el = document.createElement('button');
            el.textContent = name;
            el.setAttribute('aria-pressed', String(sd.texture === tile));
            el.onclick = () => edit(`texture ${name} on ${segment} ${SIDE_NAMES[side]}`,
                                    (level) => setTexture(level, segment, side, tile));
            textures.appendChild(el);
        });
        container.appendChild(textures);

        const turns = sd.rotation ?? 0;
        button(`Rotate texture (${turns * 90}°)`,
               'Turns this wall’s texture a further 90 degrees',
               () => edit(`rotate texture to ${((turns + 1) % 4) * 90}° on ${segment} ` +
                          `${SIDE_NAMES[side]}`,
                          (level) => setRotation(level, segment, side, turns + 1)));

        uvModeField(container, 'layout', UV_MODE_TITLE, sd.uvMode ?? null,
                    app.level.uvMode ?? 'side',
                    (mode) => edit(`layout ${mode ?? 'level'} on ${segment} ${SIDE_NAMES[side]}`,
                                   (level) => setUvMode(level, segment, side, mode)));
        uvModeField(container, 'level layout',
                    'The layout of every side and grate that does not set its own',
                    app.level.uvMode ?? null, null,
                    (mode) => edit(`level layout ${mode ?? 'side'}`,
                                   (level) => setLevelUvMode(level, mode)));

        // How brightly the side emits on top of the light that reaches it.
        const emits = document.createElement('label');
        emits.className = 'field';
        emits.append('emits');
        emits.title = 'Brightness the side adds on its own. 0 is an ordinary wall; ' +
                      'around 1 reads as lit, higher for lava.';
        const input = document.createElement('input');
        Object.assign(input, { type: 'number', min: 0, max: 8, step: 0.1 });
        input.value = sd.emissive ?? 0;
        input.onchange = () => edit(`emissive ${input.value} on ${segment} ${SIDE_NAMES[side]}`,
                                    (level) => setEmissive(level, segment, side, Number(input.value)));
        emits.appendChild(input);
        container.appendChild(emits);

        // An emitting side is bright, and lights nothing. The renderer has no
        // area lights, so the room around a lava pool stays as black as it was
        // unless something in the segment actually casts. Offering the light
        // here is the difference between the feature looking broken and looking
        // like what the author meant; it is the same pairing as a lamp and its
        // fixture.
        if (sd.emissive > 0 && !app.level.lights.some((l) => l.segment === segment)) {
            button('Light this room too',
                   'Drops a point light at the middle of this side, tinted like the glow. ' +
                   'An emitting side is bright but casts nothing on its own.',
                   () => edit(`light for the glow in ${segment}`, (level) => {
                       const c = sideCorners(level, segment, side);
                       const middle = c.reduce((a, b) => add(a, b)).map((v) => v / 4);
                       // Off the surface rather than on it: a light exactly in
                       // the plane of the wall it is meant to light grazes it
                       // and lights nothing. sd.normal points into the segment.
                       const inward = scale(level.segments[segment].sides[side].normal, 3);
                       addLight(level, add(middle, inward), segment);
                   }));
        }
        return;
    }

    if (app.mode === 'vertex') {
        const chosen = selectionItems(selection).filter(isGeometry);
        if (chosen.length === 0) {
            return hint('Click a vertex, shift+click to add more. Drag the gizmo to move them; ' +
                        'W/E/R switch it between move, turn and scale; Q points it along the selection.');
        }
        if (chosen.length > 1) {
            const segments = new Set();
            for (const { index } of chosen) {
                app.level.segments.forEach((seg, si) => {
                    if (seg.verts.includes(index)) segments.add(si);
                });
            }
            hint(`${chosen.length} vertices across ${segments.size} segment(s)`);
            if (chosen.length === 2) {
                // The one clicked last moves onto the one clicked first, so the
                // corner already in place is the one to pick first.
                const [onto, from] = chosen.map((item) => item.index);
                button('Snap together',
                       `Moves vertex ${from} onto vertex ${onto}, the one selected first, ` +
                       'so two walls can then be joined',
                       () => edit(`snap vertex ${from} onto ${onto}`,
                                  (level) => snapVertex(level, from, onto)));
            }
            button('Clear', 'Deselect all of them (Escape)',
                   () => { clearSelection(); renderTools(); renderSelection(); });
            return;
        }
        const from = chosen[0].index;
        const using = app.level.segments.filter((seg) => seg.verts.includes(from)).length;
        hint(`vertex ${from}, used by ${using} segment(s)`);
        // For when the vertex to snap onto sits under this one's gizmo and
        // cannot be clicked.
        const onto = nearestSnapTarget(app.level, from);
        if (onto >= 0) {
            const away = distance(app.level.vertices[from], app.level.vertices[onto]);
            button(`Snap to nearest vertex (${onto}, ${away.toFixed(2)} away)`,
                   'Moves this vertex onto the closest vertex outside its own segments, ' +
                   'so two walls can then be joined',
                   () => edit(`snap vertex ${from} onto ${onto}`,
                              (level) => snapVertex(level, from, onto)));
        }
        return;
    }

    // --- entity mode --------------------------------------------------------
    renderEntityTools(container, button, hint);
}

// Everything placed inside a mine. New things land in the centre of the
// segment the author last had selected, because aiming a click into a room you
// are looking at from outside is guesswork.
function renderEntityTools(container, button, hint) {
    const selection = app.selection;
    const target = app.lastSegment;

    const row = (label) => {
        const el = document.createElement('div');
        el.className = 'hint';
        el.textContent = label;
        container.appendChild(el);
        return el;
    };

    // One button per wall tile, the current one pressed, the way the side
    // panel's texture picker lays them out. -1 presses none.
    const tileRow = (current, onpick, title) => {
        const tiles = document.createElement('div');
        tiles.className = 'textures';
        WALL_TILE_NAMES.forEach((name, tile) => {
            const el = document.createElement('button');
            el.textContent = name;
            el.title = title;
            el.setAttribute('aria-pressed', String(current === tile));
            el.onclick = () => onpick(tile, name);
            tiles.appendChild(el);
        });
        container.appendChild(tiles);
    };

    const field = (label, value, attrs, onchange) => {
        const wrap = document.createElement('label');
        wrap.className = 'field';
        wrap.append(label);
        const input = document.createElement('input');
        Object.assign(input, attrs);
        input.value = value;
        input.onchange = () => onchange(input);
        wrap.appendChild(input);
        container.appendChild(wrap);
        return input;
    };

    // The three fields every emissive thing has. Both prop kinds carry a light
    // with the same meaning as a static one's - radius is the flood fill's
    // cutoff, not a falloff curve - so they get the same controls.
    const lightFields = (entity, set) => {
        const hex = '#' + entity.color.map((c) =>
            Math.round(Math.min(Math.max(c, 0), 1) * 255).toString(16).padStart(2, '0')).join('');
        field('colour', hex, { type: 'color' }, (el) => set('colour', () => {
            const v = el.value;
            entity.color = [1, 3, 5].map((i) => parseInt(v.slice(i, i + 2), 16) / 255);
        }));
        field('light radius', entity.lightRadius, { type: 'number', min: 1, max: 400, step: 1 },
              (el) => set('light radius', () => { entity.lightRadius = Number(el.value); }));
        field('light intensity', entity.lightIntensity,
              { type: 'number', min: 0, max: 40, step: 0.1 },
              (el) => set('light intensity', () => { entity.lightIntensity = Number(el.value); }));
    };

    if (selection && MOVABLE.has(selection.kind)) {
        const kind = selection.kind;
        const index = selection.index;

        if (kind === 'light') {
            const light = app.level.lights[index];
            field('radius', light.radius, { type: 'number', min: 1, max: 400, step: 1 },
                  (el) => edit(`light ${index} radius`, () => { light.radius = Number(el.value); }));
            field('intensity', light.intensity, { type: 'number', min: 0, max: 40, step: 0.1 },
                  (el) => edit(`light ${index} intensity`, () => { light.intensity = Number(el.value); }));
            const hex = '#' + light.color.map((c) =>
                Math.round(Math.min(Math.max(c, 0), 1) * 255).toString(16).padStart(2, '0')).join('');
            field('colour', hex, { type: 'color' }, (el) => edit(`light ${index} colour`, () => {
                const v = el.value;
                light.color = [1, 3, 5].map((i) => parseInt(v.slice(i, i + 2), 16) / 255);
            }));
            button(light.type === 'spot' ? 'Make it a point light' : 'Make it a spot light',
                   'A spot also writes a direction and a cone angle',
                   () => edit(`light ${index} type`, () => {
                       light.type = light.type === 'spot' ? 'point' : 'spot';
                   }));
            if (light.type === 'spot') {
                field('cone°', light.coneDegrees, { type: 'number', min: 1, max: 179, step: 1 },
                      (el) => edit(`light ${index} cone`, () => { light.coneDegrees = Number(el.value); }));
            }

            // A visible source, so the light does not appear to come from
            // nowhere. The same emissive quad a swinging lamp's housing is.
            const fitted = light.fixtureSize > 0;
            button(fitted ? 'Remove the fixture' : 'Add a fixture',
                   'Draws an emissive marker at the light, the way a swinging lamp draws its ' +
                   'housing. A spotlight with nothing at its apex reads as light from nowhere.',
                   () => edit(`light ${index} fixture`, () => {
                       light.fixtureSize = fitted ? 0 : LIGHT_FIXTURE_SIZE;
                   }));
            if (fitted) {
                field('fixture size', light.fixtureSize,
                      { type: 'number', min: 0.1, max: 8, step: 0.1 },
                      (el) => edit(`light ${index} fixture size`, () => {
                          light.fixtureSize = Number(el.value);
                      }));
                row('half the marker’s width, so 0.9 draws 1.8 units across');
            }
        }

        if (kind === 'object' && app.level.objects[index].type === BLOCK_TYPE) {
            // Size rather than scale: a block is edited by its edges, one axis
            // at a time, in the block's own frame. `scale` is still honoured
            // if a file carries one, but the panel does not offer both knobs.
            const block = app.level.objects[index];
            ['x', 'y', 'z'].forEach((axis, k) => {
                field(`size ${axis}`, block.size[k], { type: 'number', min: 0.1, max: 400, step: 0.5 },
                      (el) => edit(`block ${index} size ${axis}`, () => {
                          const value = Number(el.value);
                          if (value > 0) block.size[k] = value;
                      }));
            });
            row(`texture: ${WALL_TILE_NAMES[block.texture] ?? block.texture}`);
            tileRow(block.texture, (tile, name) => edit(`block ${index} tile ${name}`, () => {
                block.texture = tile;
            }), 'use this tile on every face of the block');
            const stretched = block.textureMode === 'stretch';
            button(stretched ? 'Repeat the texture' : 'Stretch the texture',
                   stretched
                       ? 'Tile it every 5.5 units, the same density as the walls'
                       : 'One copy of the tile across each face, whatever its size',
                   () => edit(`block ${index} texture ${stretched ? 'repeat' : 'stretch'}`, () => {
                       block.textureMode = stretched ? 'repeat' : 'stretch';
                   }));
            row(stretched ? 'one tile per face' : 'one tile every 5.5 units, as on the walls');
        } else if (kind === 'object') {
            const object = app.level.objects[index];
            field('scale', object.scale, { type: 'number', min: 0.1, max: 20, step: 0.1 },
                  (el) => edit(`object ${index} scale`, () => { object.scale = Number(el.value); }));
        }

        if (kind === 'object') {
            const object = app.level.objects[index];
            field('health', object.health,
                  { type: 'number', min: 0, max: 5000, step: 1 },
                  (el) => edit(`object ${index} health`, () => { object.health = Number(el.value); }));
            row('health 0 is indestructible scenery; anything above makes it a target');
        }

        if (kind === 'ball') {
            const ball = app.level.floatingLights[index];
            const set = (what, value) => edit(`floating light ${index} ${what}`, value);
            field('radius', ball.radius, { type: 'number', min: 0.2, max: 6, step: 0.1 },
                  (el) => set('radius', () => { ball.radius = Number(el.value); }));
            field('mass', ball.mass, { type: 'number', min: 0.05, max: 20, step: 0.05 },
                  (el) => set('mass', () => { ball.mass = Number(el.value); }));
            row('every impulse is divided by mass: 0.45 means a missile throws it across the room');
            field('bounce', ball.restitution, { type: 'number', min: 0, max: 1, step: 0.02 },
                  (el) => set('bounce', () => { ball.restitution = Number(el.value); }));
            row('0 makes it slide along a wall like a ship; 0.72 makes it rattle around the room');
            field('drift', ball.drift, { type: 'number', min: 0, max: 12, step: 0.1 },
                  (el) => set('drift', () => { ball.drift = Number(el.value); }));
            row('how fast it wanders when nothing has hit it');
            lightFields(ball, set);
        }

        if (kind === 'lamp') {
            const lamp = app.level.swingingLights[index];
            const set = (what, value) => edit(`swinging light ${index} ${what}`, value);
            field('cord', lamp.cordLength, { type: 'number', min: 0.5, max: 60, step: 0.5 },
                  (el) => set('cord', () => { lamp.cordLength = Number(el.value); }));
            // Radians in the file, degrees in the panel: nobody authors a swing
            // in radians, and the arc in the viewport is the real readout.
            field('swing°', +(lamp.amplitude * 180 / Math.PI).toFixed(1),
                  { type: 'number', min: 0, max: 85, step: 1 },
                  (el) => set('swing', () => { lamp.amplitude = Number(el.value) * Math.PI / 180; }));
            field('rate', lamp.rate, { type: 'number', min: 0.05, max: 8, step: 0.05 },
                  (el) => set('rate', () => { lamp.rate = Number(el.value); }));
            row(`one full swing takes ${(2 * Math.PI / Math.max(0.05, lamp.rate)).toFixed(2)} s`);
            field('phase', lamp.phase, { type: 'number', min: 0, max: 6.3, step: 0.1 },
                  (el) => set('phase', () => { lamp.phase = Number(el.value); }));
            row('offsets two lamps in one room so they do not swing in lockstep');
            const axisRow = document.createElement('div');
            axisRow.className = 'textures';
            for (const [label, axis] of [['swing along X', [1, 0, 0]], ['swing along Z', [0, 0, 1]]]) {
                const el = document.createElement('button');
                el.textContent = label;
                el.onclick = () => set('axis', () => { lamp.swingAxis = axis.slice(); });
                axisRow.appendChild(el);
            }
            container.appendChild(axisRow);
            field('cone°', lamp.coneDegrees, { type: 'number', min: 1, max: 179, step: 1 },
                  (el) => set('cone', () => { lamp.coneDegrees = Number(el.value); }));
            lightFields(lamp, set);
        }

        if (kind === 'pickup') {
            const pickup = app.level.pickups[index];
            if (pickup.kind >= PICKUP_KEY_FIRST) {
                row('keys appear at every difficulty');
            } else {
                row('appears up to');
                const difficultyRow = document.createElement('div');
                difficultyRow.className = 'textures';
                DIFFICULTY_NAMES.forEach((name, difficulty) => {
                    const el = document.createElement('button');
                    el.textContent = name;
                    el.title = difficulty === DIFFICULTY_NAMES.length - 1
                        ? 'on every difficulty'
                        : `on ${DIFFICULTY_NAMES.slice(0, difficulty + 1).join(' and ')} only`;
                    el.setAttribute('aria-pressed', String(pickup.maxDifficulty === difficulty));
                    el.onclick = () => edit(`pickup ${index} up to ${name}`, () => {
                        pickup.maxDifficulty = difficulty;
                    });
                    difficultyRow.appendChild(el);
                });
                container.appendChild(difficultyRow);
            }
        }

        if (kind === 'exit') {
            const exit = app.level.exitTrigger;
            field('radius', exit.radius, { type: 'number', min: 1, max: 60, step: 0.5 },
                  (el) => edit('exit radius', () => { exit.radius = Number(el.value); }));
        }

        // Offered beside Delete, and only for the kinds a level can hold more
        // than one of. The start and the exit are the level rather than
        // furniture in it, and there is one of each.
        if (ENTITY_LISTS[kind]) {
            button(`Duplicate ${kind}`,
                   'A copy with the same settings, in the same place (Ctrl+D). The copy becomes ' +
                   'the selection, so the gizmo is on it and the first drag moves it off the ' +
                   'original.',
                   () => duplicateSelected());
        }

        if (kind !== 'start') {
            button(`Delete ${kind}`, '', () => edit(`delete ${kind}`, (level) => {
                removeEntity(level, kind, index);
                app.selection = null;
                gizmo.detach();
            }));
        } else {
            row('the player start cannot be deleted');
        }
        return;
    }

    if (selection?.kind === 'side') {
        const { segment, side } = selection;
        const sd = app.level.segments[segment].sides[side];
        if (sd.neighbour >= 0 && sd.door < 0 && sd.grate < 0) {
            const keys = availableKeys(app.level);
            row(`portal → ${sd.neighbour}`);
            for (let key = -1; key < KEY_NAMES.length; ++key) {
                const name = key < 0 ? 'no key' : KEY_NAMES[key];
                // Offered even when the key is not placed: the level script
                // can hand it over with give_key(). The tooltip says so, and
                // validation warns when there is no script to do it.
                const has = key < 0 || keys.has(key);
                button(`Add door (${name})`,
                    has ? '' : `no ${name} key is placed in this level; it opens only if the ` +
                               'level script gives one',
                    () => edit(`door on ${segment} ${SIDE_NAMES[side]}`,
                               (level) => addDoor(level, segment, side, key)));
            }
            // Offered apart from the keys rather than as a fifth one, because
            // it is not a key: it is the other lock, and it stacks with them.
            // Gated on there being a reactor: without one the hatch never
            // opens, and no script can change that.
            {
                const reactor = hasReactor(app.level);
                const el = button('Add emergency hatch',
                    reactor ? 'sealed until the reactor is destroyed'
                            : 'this level has no reactor, so a hatch could never open',
                    () => edit(`hatch on ${segment} ${SIDE_NAMES[side]}`,
                               (level) => addDoor(level, segment, side, -1, true)));
                el.disabled = !reactor;
            }

            // A grate instead of a door: one button per tile, because the
            // tile is the grate - its alpha is where the holes are.
            row('add a grate');
            tileRow(-1, (tile, name) => edit(`grate ${name} on ${segment} ${SIDE_NAMES[side]}`,
                                             (level) => addGrate(level, segment, side, tile)),
                    'a grate of this tile: seen, lit and shot through, not flown through. ' +
                    'The tile\u2019s transparent pixels are the holes.');
            return;
        }
        if (sd.grate >= 0) {
            const index = sd.grate;
            const grate = app.level.grates[index];
            row(`grate ${index}, ${WALL_TILE_NAMES[grate.texture] ?? grate.texture}`);
            tileRow(grate.texture, (tile, name) => edit(`grate ${index} tile ${name}`,
                                                        (level) => setGrateTexture(level, index, tile)),
                    'use this tile for the grate');
            uvModeField(container, 'layout',
                        'side fits the tile to this portal; world continues the grid of ' +
                        'world-laid walls around it. Shown in the game, not here.',
                        grate.uvMode ?? null, app.level.uvMode ?? 'side',
                        (mode) => edit(`grate ${index} layout ${mode ?? 'level'}`,
                                       (level) => setGrateUvMode(level, index, mode)));
            button('Delete grate', '', () => edit(`delete grate ${index}`,
                                                  (level) => removeEntity(level, 'grate', index)));
            return;
        }
        if (sd.door >= 0) {
            const door = app.level.doors[sd.door];
            const lock = door.key < 0 ? 'none' : KEY_NAMES[door.key];
            row(`door ${sd.door}, key ${lock}${door.hatch ? ', emergency hatch' : ''}`);
            button('Delete door', '', () => edit(`delete door ${sd.door}`,
                                                 (level) => removeEntity(level, 'door', sd.door)));
            return;
        }
        return hint('That side is a solid wall. A door or a grate has to be a portal.');
    }

    if (target === null || target === undefined) {
        return hint('Select a segment first — new things are placed in its centre.');
    }

    row(`placing into segment ${target}`);
    button('Add light', '', () => edit('add light', (level) => {
        const index = addLight(level, segmentCentre(level, target));
        app.selection = { kind: 'light', index };
    }));

    button('Add floating light', 'a light in a ball: shoot it and it bounces around the room',
           () => edit('add floating light', (level) => {
               const index = addFloatingLight(level, segmentCentre(level, target));
               app.selection = { kind: 'ball', index };
           }));
    button('Add swinging light', 'a spotlight hung from this segment\u2019s ceiling on a cord',
           () => edit('add swinging light', (level) => {
               const { position, cordLength } = ceilingAnchor(level, target);
               const index = addSwingingLight(level, position, cordLength);
               app.selection = { kind: 'lamp', index };
           }));

    row('objects');
    const objectRow = document.createElement('div');
    objectRow.className = 'textures';
    for (const type of [BLOCK_TYPE, ...app.models.filter((t) => t !== BLOCK_TYPE)]) {
        const el = document.createElement('button');
        el.textContent = type;
        el.title = `place a ${type}`;
        el.onclick = () => edit(`add ${type}`, (level) => {
            const index = addObject(level, type, segmentCentre(level, target));
            app.selection = { kind: 'object', index };
        });
        objectRow.appendChild(el);
    }
    container.appendChild(objectRow);

    row('pickups');
    const pickupRow = document.createElement('div');
    pickupRow.className = 'textures';
    PICKUP_NAMES.forEach((name, kind) => {
        const el = document.createElement('button');
        el.textContent = name.replace('key_', '');
        el.title = `place a ${name}`;
        el.onclick = () => edit(`add ${name}`, (level) => {
            const index = addPickup(level, kind, segmentCentre(level, target));
            app.selection = { kind: 'pickup', index };
        });
        pickupRow.appendChild(el);
    });
    container.appendChild(pickupRow);

    if (!app.level.exitTrigger) {
        button('Add exit', 'Where the level is escaped from, once the reactor is gone',
               () => edit('add exit', (level) => {
                   setExit(level, segmentCentre(level, target));
                   app.selection = { kind: 'exit', index: 0 };
               }));
    }
}

function setModeButtons() {
    for (const button of $('modes').querySelectorAll('button')) {
        button.setAttribute('aria-pressed', String(button.dataset.mode === app.mode));
    }
    view.groups.vertices.visible = app.mode === 'vertex';
}

// --- panel ------------------------------------------------------------------

function renderStats() {
    const level = app.level;
    let portals = 0;
    for (const seg of level.segments) for (const sd of seg.sides) if (sd.neighbour >= 0) ++portals;

    const rows = [
        ['name', level.name],
        ['vertices', level.vertices.length],
        ['segments', level.segments.length],
        ['portals', portals / 2],
        ['wall triangles', level.walls.length],
        ['lights', level.lights.length],
        ['objects', level.objects.length],
        ['doors', level.doors.length],
        ['grates', level.grates.length],
        ['pickups', level.pickups.length],
    ];
    $('stats').innerHTML = rows.map(([k, v]) => `<dt>${k}</dt><dd>${v}</dd>`).join('');
}

function renderHistoryButtons() {
    $('undo').disabled = !app.history.canUndo();
    $('redo').disabled = !app.history.canRedo();
    $('undo').title = app.history.canUndo() ? `Ctrl+Z — ${app.history.undoLabel()}` : 'Ctrl+Z';
    $('redo').title = app.history.canRedo() ? `Ctrl+Shift+Z — ${app.history.redoLabel()}` : 'Ctrl+Shift+Z';
    renderDirty();
}

function renderFindings() {
    const { findings, closure } = app.validation;
    const container = $('findings');
    container.innerHTML = '';

    const errors = findings.filter((f) => f.severity === 'error').length;
    $('findings-title').textContent = errors > 0 ? `Checks — ${errors} error(s)` : 'Checks';

    if (findings.length === 0) {
        const euler = closure ? closure.weldedVertices - closure.edges + closure.triangles : '?';
        container.innerHTML =
            `<div class="clean">structure valid · wall surface closed (V-E+F = ${euler})</div>`;
        return;
    }

    for (const f of findings) {
        const el = document.createElement('div');
        el.className = `finding ${f.severity}`;
        const only = EDITOR_ONLY_RULES.has(f.rule) ? ' · editor only' : '';
        el.innerHTML = `<div class="rule">${f.rule}${only}</div><div>${escapeHtml(f.message)}</div>`;
        if (f.segment !== undefined && f.segment >= 0) {
            el.classList.add('clickable');
            el.title = `show segment ${f.segment}`;
            el.onclick = () => selectSegment(f.segment, true);
        }
        container.appendChild(el);
    }
}

function renderSelection() {
    const container = $('selection');
    const selection = app.selection;
    if (!selection) {
        container.textContent = 'Click a wall or a gizmo. Shift+click to add to the selection.';
        view.highlightSegments([]);
        return;
    }

    // More than one thing selected: describe the group and what the gizmo will
    // do to it, rather than the primary on its own. The primary still gets a
    // line, because it is the thing the author just clicked.
    const items = selectionItems(selection);
    if (items.length > 1) {
        const kinds = [...new Set(items.map((i) => i.kind))];
        const points = items.map(itemPosition).filter(Boolean);
        const middle = centroid(points).map((n) => n.toFixed(2)).join(' ');
        const what = kinds.length === 1
            ? plural(kinds[0], items.length)
            : kinds.join(' + ');
        const indices = items.map((i) => i.index).join(' ');
        const verb = { translate: 'moves', rotate: 'turns', scale: 'scales' }[gizmo.mode];
        container.innerHTML =
            `<b>${items.length} ${escapeHtml(what)}</b>\n${escapeHtml(indices)}\n` +
            `centre ${middle}\ngizmo ${verb} the group about its centre, along ` +
            `${app.gizmoSpace === 'local' ? 'its own axes' : 'the world axes'}\n` +
            `primary ${escapeHtml(selection.kind)} ${selection.index}`;
        if (items.every(isGeometry)) view.highlightVertices(items.map((i) => i.index));
        else view.highlightSegments([]);
        return;
    }

    if (selection.kind === 'side') {
        const { segment, side } = selection;
        const sd = app.level.segments[segment].sides[side];
        const what = sd.neighbour >= 0
            ? `portal → segment ${sd.neighbour}${sd.door >= 0 ? `, door ${sd.door}` : ''}` +
              `${sd.grate >= 0 ? `, grate ${sd.grate}` : ''}`
            : `wall, ${WALL_TILE_NAMES[sd.texture] ?? sd.texture}`;
        container.innerHTML =
            `<b>segment ${segment}, ${SIDE_NAMES[side]}</b>\n${what}\n` +
            `depth through here ${segmentDepth(app.level, segment, side).toFixed(1)}`;
        view.highlightSide(segment, side);
        return;
    }

    if (selection.kind === 'vertex') {
        const p = app.level.vertices[selection.index];
        const using = [];
        app.level.segments.forEach((seg, si) => { if (seg.verts.includes(selection.index)) using.push(si); });
        container.innerHTML =
            `<b>vertex ${selection.index}</b>\n${p.map((n) => n.toFixed(2)).join(' ')}\n` +
            `used by segment(s) ${using.join(' ')}`;
        view.highlightVertex(selection.index);
        return;
    }

    if (selection.kind === 'segment') {
        const si = selection.index;
        const seg = app.level.segments[si];
        const sides = seg.sides.map((sd, i) => {
            if (sd.neighbour < 0) return `  ${SIDE_NAMES[i].padEnd(6)} wall, ${WALL_TILE_NAMES[sd.texture] ?? sd.texture}`;
            const door = sd.door >= 0 ? `, door ${sd.door}` : '';
            const grate = sd.grate >= 0 ? `, grate ${sd.grate}` : '';
            return `  ${SIDE_NAMES[i].padEnd(6)} portal → ${sd.neighbour}${door}${grate}`;
        }).join('\n');
        const c = seg.center.map((n) => n.toFixed(1)).join(' ');
        container.innerHTML = `<b>segment ${si}</b>\ncentre ${c}\nverts ${seg.verts.join(' ')}\n${escapeHtml(sides)}`;
        view.highlightSegments([si]);
        return;
    }

    if (selection.kind === 'light') {
        const light = app.level.lights[selection.index];
        const reach = view.lightReach(selection.index);
        const rgb = light.color.map((n) => n.toFixed(2)).join(' ');
        container.innerHTML =
            `<b>light ${selection.index}</b> (${light.type})\n` +
            `segment ${light.segment}\nradius ${light.radius}\nintensity ${light.intensity}\n` +
            `colour ${rgb}\n\nreaches ${reach.length} segment(s):\n  ${reach.join(' ')}`;
        view.highlightSegments(reach, light.segment >= 0 ? 0xffe066 : 0xe0645a);
        return;
    }

    const label = {
        object: () => {
            const o = app.level.objects[selection.index];
            const shape = o.type === BLOCK_TYPE
                ? `size ${o.size.join(' x ')}\ntexture ${WALL_TILE_NAMES[o.texture] ?? o.texture}, ` +
                  `${o.textureMode}\n`
                : `scale ${o.scale}\n`;
            return `<b>object ${selection.index}</b> ${escapeHtml(o.type)}\nsegment ${o.segment}\n` +
                   `${shape}health ${o.health}`;
        },
        pickup: () => {
            const p = app.level.pickups[selection.index];
            const upTo = p.kind >= PICKUP_KEY_FIRST ? '' : `\nup to ${DIFFICULTY_NAMES[p.maxDifficulty]}`;
            return `<b>pickup ${selection.index}</b> ${PICKUP_NAMES[p.kind]}\nsegment ${p.segment}${upTo}`;
        },
        start: () => `<b>player start</b>\nsegment ${app.level.start.segment}`,
        exit: () => `<b>exit</b>\nsegment ${app.level.exitTrigger.segment}\n` +
                    `radius ${app.level.exitTrigger.radius}`,
    }[selection.kind];

    container.innerHTML = label ? label() : selection.kind;
    const segment = {
        object: () => app.level.objects[selection.index].segment,
        pickup: () => app.level.pickups[selection.index].segment,
        start: () => app.level.start.segment,
        exit: () => app.level.exitTrigger.segment,
    }[selection.kind];
    const si = segment ? segment() : -1;
    view.highlightSegments(si >= 0 ? [si] : []);
}

function selectSegment(index, frame = false) {
    app.selection = index >= 0 ? { kind: 'segment', index } : null;
    if (index >= 0) app.lastSegment = index;
    if (frame && index >= 0) frameSegment(index);
    renderTools();
    renderSelection();
}

function escapeHtml(text) {
    return String(text).replace(/[&<>]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));
}

// --- layers -----------------------------------------------------------------

function applyLayers() {
    for (const input of $('layers').querySelectorAll('input')) {
        if (input.dataset.group === 'xray') view.setXray(input.checked);
        else if (input.dataset.group === 'textures') view.setTextured(input.checked);
        else view.setVisible(input.dataset.group, input.checked);
    }
    view.groups.vertices.visible = app.mode === 'vertex';
}

$('layers').innerHTML = LAYERS.map(([group, label, on]) =>
    `<label><input type="checkbox" data-group="${group}"${on ? ' checked' : ''}>${label}</label>`).join('');
$('layers').addEventListener('change', applyLayers);

$('gizmo-modes').innerHTML = GIZMO_MODES.map(([mode, label, key]) =>
    `<button data-gizmo="${mode}" title="${key}" aria-pressed="${mode === 'translate'}">` +
    `${label}</button>`).join('');
$('gizmo-modes').insertAdjacentHTML('beforeend',
    '<button data-space title="Q — point the handles along the selection’s own axes: a ' +
    'wall’s width, height and normal, a segment’s length, height and depth" ' +
    'aria-pressed="false">Local</button>');
$('gizmo-modes').addEventListener('click', (e) => {
    if (e.target.dataset?.space !== undefined) return setGizmoSpace(app.gizmoSpace === 'local' ? 'world' : 'local');
    const mode = e.target.dataset?.gizmo;
    if (mode) setGizmoMode(mode);
});

function setGizmoSpace(space) {
    if (gizmo.dragging) return;
    app.gizmoSpace = space;
    $('gizmo-modes').querySelector('[data-space]')
        .setAttribute('aria-pressed', String(space === 'local'));
    syncGizmo();
    renderSelection();
}

// Switching what the gizmo does re-parks it, because a mode change mid-selection
// must not carry the previous mode's pose into the next drag.
function setGizmoMode(mode) {
    gizmo.setMode(mode);
    for (const b of $('gizmo-modes').querySelectorAll('button')) {
        b.setAttribute('aria-pressed', String(b.dataset.gizmo === mode));
    }
    syncGizmo();
    renderSelection();
}

$('modes').innerHTML = MODES.map(([mode, label]) =>
    `<button data-mode="${mode}" aria-pressed="${mode === 'segment'}">${label}</button>`).join('');
$('modes').addEventListener('click', (e) => {
    const mode = e.target.closest('button')?.dataset.mode;
    if (mode) setMode(mode);
});

// --- the selection ----------------------------------------------------------

// A selection is one primary item plus, when more than one thing is selected,
// the whole list.
//
// `kind` and `index` keep naming the PRIMARY - the last thing clicked, the one
// the panel describes, the one every single-selection tool acts on - so nothing
// that already read a selection had to learn about groups. `items` appears only
// when there is more than one, which is what keeps a single click producing
// exactly the selection it produced before.
const sameItem = (a, b) => a.kind === b.kind && a.index === b.index;

function selectionItems(selection) {
    if (!selection) return [];
    if (selection.items) return selection.items.slice();
    return [{ kind: selection.kind, index: selection.index }];
}

function makeSelection(items) {
    if (items.length === 0) return null;
    const primary = items[items.length - 1];
    return items.length === 1 ? { ...primary } : { ...primary, items: items.slice() };
}

// Shift+click adds the item, or takes it back out if it was already in.
//
// A vertex and a light never end up in the same selection: one is geometry and
// the other is a thing standing in it, they are transformed through different
// functions, and a group holding both would have to answer what rotating it
// means. Shift+clicking across that line replaces rather than mixes. Entity
// kinds DO mix - moving a light and the pickup beside it together is the whole
// point - because they all move the same way.
const isGeometry = (item) => item.kind === 'vertex';

// Every kind but one pluralises by adding an s.
const plural = (kind, n) => (n === 1 ? kind : (kind === 'vertex' ? 'vertices' : `${kind}s`));

function selectItem(item, extend) {
    let items = extend ? selectionItems(app.selection) : [];
    if (items.length > 0 && isGeometry(items[0]) !== isGeometry(item)) items = [];

    const at = items.findIndex((s) => sameItem(s, item));
    if (at >= 0) items.splice(at, 1);
    else items.push(item);

    app.selection = makeSelection(items);
    syncGizmo();
}

function clearSelection() {
    app.selection = null;
    gizmo.detach();
}

// A copy of the selected entity, standing on top of it and selected in its
// place, so the gizmo lands on the copy and dragging moves it rather than the
// original.
//
// The primary only, not the whole group: one thing is what a group's primary
// means everywhere else, and a cluster of copies all sitting inside the
// cluster they were copied from is a harder thing to pull apart than it sounds.
function duplicateSelected() {
    const selection = app.selection;
    if (!selection || app.mode !== 'entity' || !ENTITY_LISTS[selection.kind]) return;
    const { kind, index } = selection;
    edit(`duplicate ${kind} ${index}`, (level) => {
        app.selection = { kind, index: duplicateEntity(level, kind, index) };
    });
}

// Selects a set of vertices and switches to vertex mode, without going through
// setMode - which clears the selection, and the whole point here is to arrive
// with one. The last index becomes the primary.
function selectVertices(indices, label) {
    const unique = [...new Set(indices)];
    if (unique.length === 0) return;
    app.mode = 'vertex';
    app.joinFrom = null;
    setModeButtons();
    view.groups.vertices.visible = true;
    app.selection = makeSelection(unique.map((index) => ({ kind: 'vertex', index })));
    syncGizmo();
    renderTools();
    renderSelection();
    status(label, 'ok');
}

// The items in the selection the gizmo can actually move, which depends on the
// mode: vertices in vertex mode, the movable kinds in entity mode, nothing
// anywhere else.
function draggableItems() {
    const items = selectionItems(app.selection);
    if (app.mode === 'vertex') return items.filter(isGeometry);
    if (app.mode === 'entity') return items.filter((s) => MOVABLE.has(s.kind));
    return [];
}

function itemPosition(item) {
    return item.kind === 'vertex'
        ? app.level.vertices[item.index]
        : entityPosition(item.kind, item.index);
}

// The axes local space points the handles along, as a frame (see transform.js),
// or null to fall back to world. Worked out from the geometry every time the
// gizmo is parked, so after a turn the handles follow the wall to where it
// now faces.
//
// A side or a segment is recognised by its vertices and given the axes the
// loader itself gives it: the side's across, up and inward normal, or the
// segment's left-right, bottom-top and back-front. Anything else is fitted to
// how its points spread, which is right for a flat set and at least stable for
// the rest; fitting a side the same way would pick arbitrary axes on a square
// wall, whose spread is the same every way across it.
function selectionFrame(items, points) {
    if (items.length === 1) {
        const item = items[0];
        if (item.kind === 'vertex') return null;
        const facing = entityFacing(app.level, item.kind, item.index);
        return facing?.length === 4 ? fromLevelQuat(facing) : null;
    }
    if (items.every((item) => item.kind === 'vertex')) {
        const chosen = new Set(items.map((item) => item.index));
        const same = (verts) => verts.length === chosen.size && verts.every((v) => chosen.has(v));
        for (let si = 0; si < app.level.segments.length; ++si) {
            const seg = app.level.segments[si];
            if (chosen.size === 8 && same(seg.verts)) {
                const centre = (side) => seg.sides[side].center;
                const between = (from, to) => centre(to).map((n, k) => n - centre(from)[k]);
                return frameFromAxes(between(0, 2), between(3, 1));   // left→right, bottom→top
            }
            if (chosen.size !== 4) continue;
            for (let side = 0; side < SIDE_VERTICES.length; ++side) {
                if (!same(SIDE_VERTICES[side].map((corner) => seg.verts[corner]))) continue;
                const c = sideCorners(app.level, si, side);
                const across = [0, 1, 2].map((k) => (c[1][k] - c[0][k]) + (c[2][k] - c[3][k]));
                const up = [0, 1, 2].map((k) => (c[3][k] - c[0][k]) + (c[2][k] - c[1][k]));
                return frameFromAxes(across, up);
            }
        }
    }
    if (points.length === 2) return edgeFrame(points[0], points[1]);
    return spreadFrame(points);
}

// Parks the gizmo on the selection's centroid, in the frame of the current
// space, or detaches it when there is nothing to move.
//
// Reset every time rather than left where the last drag put it: the pose the
// gizmo reports is read as a delta from identity, so a handle still carrying the
// previous drag's rotation would apply that rotation again the moment the next
// drag started.
function syncGizmo() {
    if (!app.level) { gizmo.detach(); return; }
    const items = draggableItems();
    const points = items.map(itemPosition).filter(Boolean);
    if (points.length === 0 || points.length !== items.length) { gizmo.detach(); return; }

    const frame = app.gizmoSpace === 'local'
        ? selectionFrame(items, points) ?? IDENTITY_QUAT
        : IDENTITY_QUAT;
    handle.position.set(...centroid(points));
    handle.quaternion.set(...frame);
    handle.scale.set(1, 1, 1);
    gizmo.setSpace(app.gizmoSpace);
    gizmo.attach(handle);
}

// --- picking and dragging ---------------------------------------------------

const raycaster = new THREE.Raycaster();
const pointer = new THREE.Vector2();
let dragged = false;

function pointerAt(e) {
    const rect = renderer.domElement.getBoundingClientRect();
    pointer.x = ((e.clientX - rect.left) / rect.width) * 2 - 1;
    pointer.y = -((e.clientY - rect.top) / rect.height) * 2 + 1;
    raycaster.setFromCamera(pointer, camera);
}

// A vertex has to stay clickable whether the view is on one segment or the
// whole mine, so the pick radius scales with how far away the camera is.
const vertexThreshold = () => camera.position.distanceTo(controls.target) * 0.012;

const gridStep = () => Number($('snap').value) || 0;

const snap = (value) => {
    const step = gridStep();
    return step > 0 ? Math.round(value / step) * step : value;
};

renderer.domElement.addEventListener('pointerdown', () => { dragged = false; });
renderer.domElement.addEventListener('pointermove', (e) => { if (e.buttons) dragged = true; });

renderer.domElement.addEventListener('dblclick', (e) => {
    if (!app.level) return;
    pointerAt(e);
    const index = view.pickSegment(raycaster);
    if (index >= 0) selectSegment(index, true);
});

renderer.domElement.addEventListener('click', (e) => {
    if (dragged || !app.level || gizmo.dragging) return;
    pointerAt(e);

    if (app.mode === 'vertex') {
        const index = view.pickVertex(raycaster, vertexThreshold());
        // A shift+click that misses keeps the group. Losing eight vertices to a
        // near miss is the one thing a multi-selection must not do.
        if (index >= 0) selectItem({ kind: 'vertex', index }, e.shiftKey);
        else if (!e.shiftKey) clearSelection();
        renderTools();
        renderSelection();
        return;
    }

    // Gizmos win over walls: they are small, and a click that lands on both
    // almost always meant the gizmo.
    const entity = view.pickEntity(raycaster);
    if (entity) {
        if (app.mode === 'entity') selectItem(entity, e.shiftKey);
        else { app.selection = entity; gizmo.detach(); }
        renderTools();
        renderSelection();
        return;
    }

    if (app.mode === 'side' || app.mode === 'entity') {
        // Portals count here and nowhere else: a door goes on one, Split
        // portal acts on one, and neither is reachable if a click passes
        // through the opening to the wall behind it.
        const face = view.pickFace(raycaster, { portals: true });
        if (app.mode === 'entity' && e.shiftKey && app.selection) {
            // Shift is for gathering things. A wall is not a thing, so a
            // shift+click that lands on one leaves the group alone rather than
            // replacing it with a side.
            return;
        }
        // In entity mode a wall click is how a door gets placed and how the
        // "place into segment N" target is chosen, so it does both.
        if (face && app.mode === 'entity') app.lastSegment = face.segment;
        app.selection = face ? { kind: 'side', ...face } : null;
        gizmo.detach();
        renderTools();
        renderSelection();
        return;
    }

    selectSegment(view.pickSegment(raycaster));
});

// Dragging rebuilds the level every frame, which at a few hundred triangles is
// cheaper than maintaining an incremental path that could get the derived planes
// out of step with the vertices.
//
// Every frame applies the gizmo's pose to the positions recorded at drag START,
// never to the ones it last produced. TransformControls reports absolute state -
// the whole drag so far - so feeding its output back in would compound over the
// few hundred frames of a drag and leave a group quietly deformed by the act of
// having been moved.
let drag = null;

gizmo.addEventListener('dragging-changed', (e) => {
    if (e.value) {
        const items = draggableItems();
        if (items.length === 0) return;
        const origin = items.map((item) => itemPosition(item).slice());
        drag = {
            items,
            origin,
            pivot: centroid(origin),
            // The pose the handle was parked in. What the gizmo reports is read
            // against it: its turn since, and its scale along these axes.
            frame: [handle.quaternion.x, handle.quaternion.y,
                    handle.quaternion.z, handle.quaternion.w],
            // What each entity was facing before the drag, so a rotation
            // composes onto that rather than onto its own last frame's output.
            facing: items.map((item) => (item.kind === 'vertex'
                ? null
                : entityFacing(app.level, item.kind, item.index))),
            undo: app.history.snapshot(app.level),
        };
        return;
    }

    if (!drag) return;
    // One undo entry per drag, not per frame - and none at all for a drag that
    // ended where it started, which is what a click on a gizmo arrow is.
    //
    // Facings are compared as well as positions, because the one thing a drag
    // can change without moving anything is where a single object is pointing:
    // rotating it about its own centre leaves its position exactly where it was.
    const changed = drag.items.some((item, i) => {
        const now = itemPosition(item);
        if (!now || drag.origin[i].some((n, k) => Math.abs(n - now[k]) > 1e-9)) return true;
        const facing = item.kind === 'vertex'
            ? null
            : entityFacing(app.level, item.kind, item.index);
        return !!facing && !!drag.facing[i]
            && facing.some((n, k) => Math.abs(n - drag.facing[i][k]) > 1e-9);
    });
    if (changed) {
        app.history.past.push({ text: drag.undo, label: dragLabel(drag.items) });
        app.history.future.length = 0;
    }
    drag = null;
    app.validation = validateLevel(app.level);
    renderFindings();
    renderHistoryButtons();
    // Re-centre on wherever the group ended up, so the next drag starts from
    // the new middle rather than from the old one.
    syncGizmo();
    renderSelection();
});

function dragLabel(items) {
    const verb = { translate: 'move', rotate: 'rotate', scale: 'scale' }[gizmo.mode] ?? 'move';
    if (items.length === 1) {
        return items[0].kind === 'vertex'
            ? `${verb} vertex ${items[0].index}`
            : `${verb} ${items[0].kind}`;
    }
    const kinds = new Set(items.map((s) => s.kind));
    const what = kinds.size === 1 ? plural([...kinds][0], items.length) : 'things';
    return `${verb} ${items.length} ${what}`;
}

// Grid snap, and the one place a group behaves differently from a single point.
//
// A single point snaps its own position, which is what a grid is for: "put this
// corner on the grid". Doing that to every member of a group would round
// distinct vertices onto the SAME grid point and collapse the shape the author
// was moving, so a group snaps the STEP it takes instead and keeps its internal
// offsets exactly. For one point the two are the same thing, because the pivot
// is the point.
//
// In local space the step is snapped along the handles' own axes instead. A
// wall moved along its normal and then rounded to the world grid would be
// pulled sideways off the arrow it was dragged by, and so would a single
// point snapped to a grid the arrow does not run along.
function translationDelta() {
    const raw = [handle.position.x, handle.position.y, handle.position.z];
    const delta = raw.map((n, k) => n - drag.pivot[k]);
    if (gridStep() <= 0) return delta;
    if (app.gizmoSpace === 'local') {
        const along = rotateVector(conjugateQuat(drag.frame), delta).map(snap);
        return rotateVector(drag.frame, along);
    }
    if (drag.items.length === 1) return raw.map(snap).map((n, k) => n - drag.pivot[k]);
    return delta.map(snap);
}

gizmo.addEventListener('objectChange', () => {
    if (!drag) return;

    const pose = [handle.quaternion.x, handle.quaternion.y, handle.quaternion.z, handle.quaternion.w];
    const transform = groupTransform({
        pivot: drag.pivot,
        translate: translationDelta(),
        // The turn since the drag began. The handle starts in drag.frame, not
        // at identity, in local space.
        rotation: multiplyQuat(pose, conjugateQuat(drag.frame)),
        scale: [handle.scale.x, handle.scale.y, handle.scale.z],
        frame: drag.frame,
    });
    const turned = transform.rotation.some((n, k) => Math.abs(n - IDENTITY_QUAT[k]) > 1e-9);
    const moved = transformPoints(transform, drag.origin);

    const vertices = [];
    const entities = [];
    drag.items.forEach((item, i) => {
        if (item.kind === 'vertex') {
            vertices.push([item.index, moved[i]]);
            return;
        }
        entities.push({
            kind: item.kind,
            index: item.index,
            position: moved[i],
            rotation: turned ? transform.rotation : null,
            facing: drag.facing[i],
        });
    });

    // One rebuild for the whole group, after every member has been written.
    if (vertices.length > 0) moveVertices(app.level, vertices);
    if (entities.length > 0) moveEntities(app.level, entities);

    view.build(app.level);
    applyLayers();
    renderSelection();
});

// The kinds the gizmo can drag. A door is not one: it belongs to a portal, not
// to a place.
const MOVABLE = new Set(['light', 'pickup', 'object', 'ball', 'lamp', 'start', 'exit']);

function entityPosition(kind, index) {
    if (kind === 'start') return app.level.start.position;
    if (kind === 'exit') return app.level.exitTrigger?.position ?? null;
    if (!ENTITY_LISTS[kind]) return null;
    return entityList(app.level, kind)[index]?.position ?? null;
}

// --- wiring -----------------------------------------------------------------

const guard = (fn) => async (...args) => {
    try {
        await fn(...args);
    } catch (e) {
        status(e.message, 'error');
        console.error(e);
    }
};

$('top-view').addEventListener('click', () => setTopDown(!topDown));

$('new-level').addEventListener('click', guard(async () => {
    if (app.level && app.history.isDirty(app.level) &&
        !window.confirm(`${app.name} has unsaved changes. Discard them?`)) return;

    const name = window.prompt('Name for the new level (it becomes assets/levels/<name>.json)');
    if (name === null) return;

    const { levels } = await listLevels();
    const complaint = checkName(name, levels);
    if (complaint) throw new Error(complaint);

    // Written before it is loaded, so "New" and "open an existing level" are
    // the same path from here on and the file exists for Verify to run against.
    await createAndOpen(name, newLevel(name), `created ${name} — one room; grow it with Extrude`);
}));

// Writes a level to assets/levels/ and opens it, which is the tail every way
// of making a level shares. New and Generate both go through it so that from
// the moment a level exists, nothing downstream knows how it was made.
async function createAndOpen(name, level, note) {
    const response = await fetch(`/api/levels/${encodeURIComponent(name)}`, {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: emitJson(serializeLevel(level)),
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error ?? `could not create ${name}`);

    const refreshed = await listLevels();
    $('level-select').innerHTML = refreshed.levels.map((n) => `<option>${n}</option>`).join('');
    await loadLevel(name);
    $('level-select').value = name;
    setMode('side');
    selectSegment(0);
    status(note, 'ok');
}

const generateDialog = $('generate-dialog');

$('generate-level').addEventListener('click', guard(() => {
    if (app.level && app.history.isDirty(app.level) &&
        !window.confirm(`${app.name} has unsaved changes. Discard them?`)) return;
    generateDialog.showModal();
}));

$('gen-cancel').addEventListener('click', () => generateDialog.close());

// Is the author content to lose what is already under this name?
//
// Asked at all because re-rolling is what Generate... is for: refusing an
// existing name, which is right for New..., made the second roll of a session
// impossible. Asked HARDER when the level was not itself a roll, because the
// name field is one typo away from `mine01` and there is no undo across a
// file write. Every generated mine carries `GENERATED_MARK` in its comment, so
// "a previous roll" and "something somebody built" are distinguishable rather
// than assumed.
async function confirmReplace(name) {
    let generated = false;
    try {
        const response = await fetch(`/api/levels/${encodeURIComponent(name)}`);
        if (response.ok) {
            const existing = await response.json();
            generated = (existing.comment ?? '').startsWith(GENERATED_MARK);
        }
    } catch {
        // Unreadable is not evidence that it is disposable. Ask the careful
        // question instead of the casual one.
    }

    if (generated) return window.confirm(`Replace ${name} with a new roll?`);
    return window.confirm(
        `${name} was not made by the generator - it looks hand-built.\n\n` +
        `Generating will replace the whole level, and nothing here can undo a ` +
        `file that has been written.\n\nReplace ${name}?`);
}

$('generate-form').addEventListener('submit', guard(async (e) => {
    // The dialog closes itself on submit; stop that until the roll has either
    // produced a mine or explained why it could not, so a failure is readable
    // with the settings that caused it still on screen.
    e.preventDefault();

    const name = $('gen-name').value.trim();
    const complaint = checkLevelName(name);
    if (complaint) throw new Error(complaint);

    // The name being taken is a question here, not a refusal.
    const { levels } = await listLevels();
    const replacing = levels.includes(name);
    if (replacing && !(await confirmReplace(name))) return;

    const go = $('gen-go');
    go.disabled = true;
    status(`generating ${name}…`);
    try {
        // Synchronous and in the browser: a roll is milliseconds, and the
        // re-roll loop inside is what makes "it validates" true before the
        // file exists rather than after.
        const { level, report } = generateMine(name, {
            rooms: Number($('gen-rooms').value),
            loops: Number($('gen-loops').value),
            keys: Number($('gen-keys').value),
            robots: Number($('gen-robots').value),
            rockFraction: Number($('gen-rock').value),
            roughness: Number($('gen-rough').value),
            litRock: Number($('gen-lit').value),
            seed: Number($('gen-seed').value),
        });

        // The seed is reported rather than echoed: when the first roll is
        // rejected the mine that got written is a later one, and the number
        // worth keeping is the one that produced THIS mine.
        //
        // Left in the dialog rather than only in the status line, which the
        // next message overwrites - roll something good, roll again, and the
        // number that would bring it back is otherwise gone. Deliberately NOT
        // written into the seed field: that would make the next roll reproduce
        // this mine instead of finding a new one.
        const last = $('gen-last');
        last.textContent = `last roll: seed ${report.seed} — ${report.rooms} rooms, ` +
                           `${report.loopsClosed} ways round`;
        last.hidden = false;

        generateDialog.close();
        const rolls = report.attempts === 1 ? 'first roll' : `${report.attempts} rolls`;
        await createAndOpen(name, level,
            `${replacing ? 'replaced' : 'generated'} ${name}: ${report.rooms} rooms, ` +
            `${report.loopsClosed} ways round, ${report.doors} locked door(s), ` +
            `${report.robots} robots — seed ${report.seed} (${rolls})`);
    } finally {
        go.disabled = false;
    }
}));

$('level-select').addEventListener('change', guard((e) => loadLevel(e.target.value)));
$('reload').addEventListener('click', guard(() => loadLevel(app.name)));
$('save').addEventListener('click', guard(saveLevel));
$('verify').addEventListener('click', guard(verify));
$('undo').addEventListener('click', () => replaceLevel(app.history.undo(app.level)));
$('redo').addEventListener('click', () => replaceLevel(app.history.redo(app.level)));

window.addEventListener('keydown', (e) => {
    if (e.target.tagName === 'INPUT' || e.target.tagName === 'SELECT') return;
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'z') {
        e.preventDefault();
        replaceLevel(e.shiftKey ? app.history.redo(app.level) : app.history.undo(app.level));
        return;
    }
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'd') {
        // The browser's own Ctrl+D bookmarks the page, which in an editor with
        // unsaved work is the last thing anyone meant by it.
        e.preventDefault();
        duplicateSelected();
        return;
    }
    if (e.key === '1') setMode('segment');
    if (e.key === '2') setMode('side');
    if (e.key === '3') setMode('vertex');
    if (e.key === '4') setMode('entity');
    if (e.key === 'f' && app.level) frameLevel();
    if (e.key === 't' && app.level) setTopDown(!topDown);
    if (e.key === 'w') setGizmoMode('translate');
    if (e.key === 'e') setGizmoMode('rotate');
    if (e.key === 'r') setGizmoMode('scale');
    if (e.key === 'q') setGizmoSpace(app.gizmoSpace === 'local' ? 'world' : 'local');
    if (e.key === 'Escape') { clearSelection(); app.joinFrom = null; renderTools(); renderSelection(); }
});

window.addEventListener('beforeunload', (e) => {
    if (app.level && app.history.isDirty(app.level)) e.preventDefault();
});

guard(async () => {
    const [{ levels, binary }, modelList] = await Promise.all([
        listLevels(),
        fetch('/api/models').then((r) => (r.ok ? r.json() : { models: [] })).catch(() => ({ models: [] })),
        loadTileNames(),
    ]);
    app.models = modelList.models ?? [];
    await models.loadAll(app.models);
    if (levels.length === 0) throw new Error('no levels in assets/levels/');
    $('level-select').innerHTML = levels.map((n) => `<option>${n}</option>`).join('');
    if (!binary) {
        $('verify').disabled = true;
        $('verify').title = 'no game binary found - build it with cmake --build --preset linux-debug';
    }
    await loadLevel(levels.includes('mine01') ? 'mine01' : levels[0]);
    $('level-select').value = app.name;
})();
