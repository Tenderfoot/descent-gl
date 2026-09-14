// The editor application: load a level, show it, edit its structure, save it
// back, and ask the game what it thinks of the result.
//
// Every edit goes through one path - `edit()` below - which records undo,
// applies the operation, revalidates and redraws. Nothing calls into ops.js
// directly, so there is no way to change a level and forget to re-check it.

import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { TransformControls } from 'three/addons/controls/TransformControls.js';

import { PICKUP_NAMES, SIDE_NAMES, WALL_TILE_NAMES } from './format.js';
import { History } from './history.js';
import { emitJson } from './json_emit.js';
import { parseLevel, resolveSegments, serializeLevel } from './level.js';
import {
    deleteSegment, extrudeSide, joinSides, moveVertex, segmentDepth, setTexture, splitPortal,
} from './ops.js';
import { EDITOR_ONLY_RULES, validateLevel } from './validate.js';
import { LevelView } from './view.js';

const $ = (id) => document.getElementById(id);

const LAYERS = [
    ['walls', 'Walls', true],
    ['xray', 'X-ray', false],
    ['portals', 'Portals', true],
    ['doors', 'Doors', true],
    ['lights', 'Lights', true],
    ['reach', 'Light radius', false],
    ['objects', 'Objects', true],
    ['pickups', 'Pickups', true],
    ['start', 'Start', true],
    ['exit', 'Exit', true],
];

const MODES = [['segment', 'Segment'], ['side', 'Side'], ['vertex', 'Vertex']];

const app = {
    level: null,
    name: null,
    validation: null,
    selection: null,
    mode: 'segment',
    joinFrom: null,
    lastEditDetail: null,
    history: new History(),
};

// --- scene ------------------------------------------------------------------

const viewport = $('viewport');
const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
viewport.appendChild(renderer.domElement);

const hud = document.createElement('div');
hud.id = 'hud';
hud.textContent = 'drag to orbit · click to select · double-click to fly in · 1/2/3 mode · F frame';
viewport.appendChild(hud);

const scene = new THREE.Scene();
scene.background = new THREE.Color(0x14161a);

const camera = new THREE.PerspectiveCamera(60, 1, 0.1, 5000);
const controls = new OrbitControls(camera, renderer.domElement);
controls.enableDamping = true;

const view = new LevelView();
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
    camera.aspect = w / Math.max(h, 1);
    camera.updateProjectionMatrix();
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
    camera.position.copy(center).add(new THREE.Vector3(radius, radius * 0.8, radius * 1.2));
    camera.near = Math.max(radius / 500, 0.05);
    camera.far = Math.max(camera.far, radius * 200);
    camera.updateProjectionMatrix();
    controls.update();
}

function frameLevel() {
    const box = view.bounds();
    if (box.isEmpty()) return;
    const center = box.getCenter(new THREE.Vector3());
    const radius = box.getSize(new THREE.Vector3()).length() * 0.5;
    controls.target.copy(center);
    camera.position.copy(center).add(new THREE.Vector3(radius * 0.9, radius * 0.8, radius * 1.1));
    camera.near = Math.max(radius / 1000, 0.05);
    camera.far = radius * 12;
    camera.updateProjectionMatrix();
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

        const failed = result.results.filter((r) => r.exit !== 0);
        $('verify-section').hidden = false;
        $('verify-output').textContent = result.results
            .map((r) => `$ descent ${r.argv.join(' ')}\n[exit ${r.exit}]\n${r.output.trim()}`)
            .join('\n\n');
        status(failed.length === 0 ? 'all checks passed'
                                   : `${failed.map((r) => r.check).join(', ')} failed`,
               failed.length === 0 ? 'ok' : 'error');
    } finally {
        $('verify').disabled = false;
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
                : 'Click a wall.');
        }
        const { segment, side } = selection;
        const sd = app.level.segments[segment].sides[side];

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
        return;
    }

    if (selection?.kind !== 'vertex') return hint('Click a vertex. Drag the gizmo to move it.');
    const using = app.level.segments.filter((seg) => seg.verts.includes(selection.index)).length;
    hint(`vertex ${selection.index}, used by ${using} segment(s)`);
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
        container.textContent = 'Click a wall or a gizmo.';
        view.highlightSegments([]);
        return;
    }

    if (selection.kind === 'side') {
        const { segment, side } = selection;
        const sd = app.level.segments[segment].sides[side];
        const what = sd.neighbour >= 0
            ? `portal → segment ${sd.neighbour}${sd.door >= 0 ? `, door ${sd.door}` : ''}`
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
            return `  ${SIDE_NAMES[i].padEnd(6)} portal → ${sd.neighbour}${door}`;
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
            return `<b>object ${selection.index}</b> ${escapeHtml(o.type)}\nsegment ${o.segment}\n` +
                   `scale ${o.scale}\nhealth ${o.health}`;
        },
        pickup: () => {
            const p = app.level.pickups[selection.index];
            return `<b>pickup ${selection.index}</b> ${PICKUP_NAMES[p.kind]}\nsegment ${p.segment}`;
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
        else view.setVisible(input.dataset.group, input.checked);
    }
    view.groups.vertices.visible = app.mode === 'vertex';
}

$('layers').innerHTML = LAYERS.map(([group, label, on]) =>
    `<label><input type="checkbox" data-group="${group}"${on ? ' checked' : ''}>${label}</label>`).join('');
$('layers').addEventListener('change', applyLayers);

$('modes').innerHTML = MODES.map(([mode, label]) =>
    `<button data-mode="${mode}" aria-pressed="${mode === 'segment'}">${label}</button>`).join('');
$('modes').addEventListener('click', (e) => {
    const mode = e.target.closest('button')?.dataset.mode;
    if (mode) setMode(mode);
});

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

const snap = (value) => {
    const step = Number($('snap').value) || 0;
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
        if (index >= 0) {
            app.selection = { kind: 'vertex', index };
            handle.position.set(...app.level.vertices[index]);
            gizmo.attach(handle);
        } else {
            app.selection = null;
            gizmo.detach();
        }
        renderTools();
        renderSelection();
        return;
    }

    // Gizmos win over walls: they are small, and a click that lands on both
    // almost always meant the gizmo.
    const entity = view.pickEntity(raycaster);
    if (entity) {
        app.selection = entity;
        renderTools();
        renderSelection();
        return;
    }

    if (app.mode === 'side') {
        const face = view.pickFace(raycaster);
        app.selection = face ? { kind: 'side', ...face } : null;
        renderTools();
        renderSelection();
        return;
    }

    selectSegment(view.pickSegment(raycaster));
});

// Dragging a vertex rebuilds the level every frame, which at a few hundred
// triangles is cheaper than maintaining an incremental path that could get the
// derived planes out of step with the vertices.
let dragStart = null;
gizmo.addEventListener('dragging-changed', (e) => {
    if (app.selection?.kind !== 'vertex') return;
    if (e.value) {
        dragStart = app.history.snapshot(app.level);
    } else if (dragStart !== null) {
        // One undo entry per drag, not per frame.
        app.history.past.push({ text: dragStart, label: `move vertex ${app.selection.index}` });
        app.history.future.length = 0;
        dragStart = null;
        app.validation = validateLevel(app.level);
        renderFindings();
        renderHistoryButtons();
    }
});

gizmo.addEventListener('objectChange', () => {
    if (app.selection?.kind !== 'vertex') return;
    const index = app.selection.index;
    const position = [snap(handle.position.x), snap(handle.position.y), snap(handle.position.z)];
    moveVertex(app.level, index, position);
    view.build(app.level);
    applyLayers();
    view.highlightVertex(index);
    renderSelection();
});

// --- wiring -----------------------------------------------------------------

const guard = (fn) => async (...args) => {
    try {
        await fn(...args);
    } catch (e) {
        status(e.message, 'error');
        console.error(e);
    }
};

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
    if (e.key === '1') setMode('segment');
    if (e.key === '2') setMode('side');
    if (e.key === '3') setMode('vertex');
    if (e.key === 'f' && app.level) frameLevel();
    if (e.key === 'Escape') { app.selection = null; app.joinFrom = null; gizmo.detach(); renderTools(); renderSelection(); }
});

window.addEventListener('beforeunload', (e) => {
    if (app.level && app.history.isDirty(app.level)) e.preventDefault();
});

guard(async () => {
    const { levels, binary } = await listLevels();
    if (levels.length === 0) throw new Error('no levels in assets/levels/');
    $('level-select').innerHTML = levels.map((n) => `<option>${n}</option>`).join('');
    if (!binary) {
        $('verify').disabled = true;
        $('verify').title = 'no game binary found - build it with cmake --build --preset linux-debug';
    }
    await loadLevel(levels.includes('mine01') ? 'mine01' : levels[0]);
    $('level-select').value = app.name;
})();
