// The editor application: load a level, show it, save it back, and ask the
// game what it thinks of the result.
//
// At E2 nothing here changes the level's geometry. That is deliberate - the
// round trip is proved to be a no-op before any feature exists that could make
// it stop being one.

import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';

import { PICKUP_NAMES, SIDE_NAMES, WALL_TILE_NAMES } from './format.js';
import { emitJson } from './json_emit.js';
import { parseLevel, serializeLevel } from './level.js';
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

const app = {
    level: null,
    name: null,
    validation: null,
    selection: null,
};

// --- scene ------------------------------------------------------------------

const viewport = $('viewport');
const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
viewport.appendChild(renderer.domElement);

const hud = document.createElement('div');
hud.id = 'hud';
hud.textContent = 'drag to orbit · scroll to zoom · click to select · double-click to fly in · F to frame all';
viewport.appendChild(hud);

const scene = new THREE.Scene();
scene.background = new THREE.Color(0x14161a);

const camera = new THREE.PerspectiveCamera(60, 1, 0.1, 5000);
const controls = new OrbitControls(camera, renderer.domElement);
controls.enableDamping = true;

const view = new LevelView();
scene.add(view.root);

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

// Puts the camera just outside a segment, looking at it. Zooming further in
// from here crosses the wall and gives the interior view, which is what the
// walls being DoubleSide is for.
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
    if (kind === 'ok') statusTimer = setTimeout(() => { el.textContent = ''; }, 4000);
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
    app.validation = validateLevel(app.level);
    app.selection = null;

    view.build(app.level);
    applyLayers();
    frameLevel();
    renderStats();
    renderFindings();
    renderSelection();
    $('verify-section').hidden = true;
    status(`loaded ${name}`, 'ok');
}

async function saveLevel() {
    if (!app.level) return;
    const body = emitJson(serializeLevel(app.level));
    const response = await fetch(`/api/levels/${encodeURIComponent(app.name)}`, {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body,
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error ?? `save failed (${response.status})`);
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
    $('stats').innerHTML = rows
        .map(([k, v]) => `<dt>${k}</dt><dd>${v}</dd>`)
        .join('');
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
        // The reach set is the thing that is hardest to picture from the file
        // and the easiest to get wrong, so selecting a light draws it.
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
}

$('layers').innerHTML = LAYERS.map(([group, label, on]) =>
    `<label><input type="checkbox" data-group="${group}"${on ? ' checked' : ''}>${label}</label>`).join('');
$('layers').addEventListener('change', applyLayers);

// --- picking ----------------------------------------------------------------

const raycaster = new THREE.Raycaster();
const pointer = new THREE.Vector2();
let dragged = false;

renderer.domElement.addEventListener('pointerdown', () => { dragged = false; });
renderer.domElement.addEventListener('pointermove', (e) => { if (e.buttons) dragged = true; });
function pointerAt(e) {
    const rect = renderer.domElement.getBoundingClientRect();
    pointer.x = ((e.clientX - rect.left) / rect.width) * 2 - 1;
    pointer.y = -((e.clientY - rect.top) / rect.height) * 2 + 1;
    raycaster.setFromCamera(pointer, camera);
}

renderer.domElement.addEventListener('dblclick', (e) => {
    if (!app.level) return;
    pointerAt(e);
    const index = view.pickSegment(raycaster);
    if (index >= 0) selectSegment(index, true);
});

renderer.domElement.addEventListener('click', (e) => {
    if (dragged || !app.level) return;
    pointerAt(e);

    // Gizmos win over walls: they are small, and a click that lands on both
    // almost always meant the gizmo.
    const entity = view.pickEntity(raycaster);
    if (entity) {
        app.selection = entity;
        renderSelection();
        return;
    }
    selectSegment(view.pickSegment(raycaster));
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

window.addEventListener('keydown', (e) => {
    if (e.key === 'f' && app.level) frameLevel();
    if (e.key === 'Escape') { app.selection = null; renderSelection(); }
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
