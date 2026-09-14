# The level editor

A browser tool for authoring `assets/levels/*.json`. It is not part of the
game build: no CMake target, no vcpkg entry, no node toolchain. Three.js is
vendored in `vendor/` and everything else is plain ES modules served straight
off disk.

```
tools/editor/serve.py          # then open http://127.0.0.1:8080
```

## What it is for

`mine01` is 124 vertices and 34 segments written by hand. That is roughly the
ceiling on what is authorable in a text editor, and the format's rules are the
reason: segments joined through a portal must reference *the same four vertex
indices*, not four coincident positions, and breaking that produces symptoms
(light leaking through a seam, a ship falling out of the world) that look like
renderer or physics bugs.

So the editor is segment-graph-native. It knows about segments, portals and
the shared vertex array, and from E3 the primitive operation will be *extrude a
side into a new segment*, which creates the reciprocal portal and shares the
four vertices by construction. There is no free vertex-soup mode, on purpose.

## What it is not

It is not the renderer. No stencil shadows, no falloff, no reversed-Z; the
colours are flat and deliberately unlike the game's, because this is a diagram
of a mine and reading it as a screenshot would be a mistake. Lights are drawn
as gizmos and reach previews. "How does this look" is answered by the game:

```
build/Debug/descent --segment 21 --no-ui --screenshot /tmp/shot.png
```

The in-game ImGui light editor stays. Tweaking a light under real shadowed
lighting is something a browser cannot do; structure is what this owns.

## State

E0 (viewer), E1 (validation parity), E2 (round trip and server), E3 (geometry
editing) and E4 (entities) are done. E5 adds authoring quality of life.

## Editing (E3)

Three selection modes — **1** segment, **2** side, **3** vertex.

| Tool | |
| --- | --- |
| **Extrude** | Grows a new segment out of a wall. The wall's four vertices become four of the new segment's, *by index*, so the portal is exact by construction. Depth defaults to the depth of the segment it grew from. |
| **Join** | Makes two walls that are already in the same place into one portal, by rewriting one segment's four vertex indices to the other's. It does not move geometry: if the corners are not already coincident it refuses and says how far off they are. |
| **Split portal** | Turns a portal back into two walls. |
| **Delete segment** | Seals the neighbours, reindexes the graph, the doors and every entity, and takes the lights, pickups and objects inside it with it. The player start and the exit are not furniture and are left pointing at nothing, loudly. |
| **Texture** | Per wall, from the five tiles in `procedural.h`. |
| **Vertex drag** | Moves one vertex; every segment referencing it deforms together, which is the shared vertex array doing its job. Optional grid snap, off by default. |

Mode **4** places the things inside a mine: lights (radius, intensity, colour,
point or spot), robots and scenery from `assets/models/` drawn as their real
meshes, pickups including keys, the exit trigger, and doors on portals. Drag
the gizmo to move any of them.

The rule all of it protects is that an entity's cached `segment` says which
segment actually contains it. The loader recomputes a `segment` that is missing
or out of range but **trusts one that is merely wrong** — so a light dragged
across a portal without re-resolving lights the wrong room and nothing anywhere
complains. Everything that moves something re-resolves it, against the same
`findSegment` the loader uses.

A door can only go on a portal, and the key colours offered are only the ones
the level actually has a key for: a red door in a mine with no red key is a
mine that cannot be finished.

Undo and redo are Ctrl+Z and Ctrl+Shift+Z, by snapshot through the writer and
reader — so an undone level is byte-identical to one loaded from the file.

Every edit goes through one function, `edit()` in `main.js`, which records
undo, applies the operation, revalidates and redraws. Nothing calls `ops.js`
directly, so there is no way to change a level and forget to re-check it.

### What the editor will not do for you

**It will not make a portal by moving vertices until they coincide.** That is
precisely the level file that validates, renders, walks — and leaks light.
Join rewrites indices or refuses.

**It will not offer a key colour the level has no key for.** A red door in a
mine with no red key is a mine that cannot be finished, and nothing in the game
checks for it.

**It will not hide a pinched surface.** Removing a segment from the middle of
a cluster can leave four faces meeting along one edge, which the shadow
volumes cannot use. No cleverness in the delete fixes that — the geometry
genuinely pinches — so the closure check reports it, Save refuses, and undo is
one keystroke away.

## Layout

| Path | |
| --- | --- |
| `index.html`, `src/editor.css` | the page |
| `src/format.js` | the schema, transcribed from `src/game/level.h` |
| `src/geometry.js` | the load-time derivation, ported from `splitSides()` |
| `src/level.js` | reading and writing the JSON |
| `src/json_emit.js` | the number and layout rules the level files use |
| `src/validate.js` | every structural rule, plus two the game does not check |
| `src/ops.js` | extrude, join, delete and the rest of the structural edits |
| `src/entities.js` | lights, robots, pickups, doors, the exit, the start |
| `src/obj.js` | just enough OBJ to draw the shipped models |
| `src/history.js` | undo and redo, by snapshot |
| `src/view.js` | the only module that imports three.js |
| `src/main.js` | the application |
| `serve.py` | static files, level read/write, and the Verify endpoint |
| `tests/conformance.mjs` | the editor's validator against the game's |
| `tests/ops.mjs` | the structural edits, checked for shape and not just legality |
| `tests/entities.mjs` | entity placement, and the segment-resolution rule |
| `testdata/base.json` | the three-segment level every broken variant is made from |

## Verification

```
node tools/editor/tests/conformance.mjs   # the validator, against the game's
node tools/editor/tests/ops.mjs           # the structural edits
node tools/editor/tests/entities.mjs      # placement and segment resolution
```

Both need a Linux debug build (`cmake --preset linux && cmake --build --preset
linux-debug`); without one it runs the editor half of each case and says the
game half was skipped. It checks three things:

* **Counts.** The editor's triangulation must reproduce the game's numbers for
  both shipped levels exactly — including how many sides took the alternate
  diagonal, which is the number that moves if the split rule is ported wrong.
* **Round trip.** Opening a shipped level and saving it must produce the same
  bytes.
* **Agreement.** Fifteen deliberately broken levels, one per rule. The editor
  must reject each one, the game must abort on it, and both must blame the
  same segment and side. The two `editorOnly` rules invert the assertion: the
  editor must warn and the game must load the level without complaint.

`ops.mjs` checks the edits. The one that matters is extrude: its corner
correspondence is the only place in the editor where a bug produces a level
that passes every structural rule and is still wrong — a twisted segment.
Every rule in the format is about indices, and a twist is about which index
went in which slot, so "it validates" is not evidence. So extrude runs in all
six directions and the result is checked for *shape*: eight distinct corners,
no edge shorter than a unit, every face planar, and the four shared indices
matching the wall it grew from. The correspondence table itself is derived
from the cube's corner coordinates rather than written down, because a
hand-built table is right for the side it was checked on and quietly twisted
for the other five.

## Two validators

The game's C++ is the authority; `src/validate.js` is a port of `validate()`
in `src/game/level.cpp`, the door and entity resolution that follows it in
`loadLevel()`, and the closed-surface check in `src/game/asset_check.cpp`.
Two implementations of one format will drift, and the way anyone finds out is
by authoring a level the editor called fine and the game aborts on — so the
conformance run above exists instead of relying on care. Compiling the C++ to
WASM was the other option, and was not worth dragging a toolchain into a
project that deliberately has none, for a function that is seventy lines.

**Verify** in the panel is the other half of that: it runs the real binary
(`--world-test`, `--collision-test`, `--loop-test`) over the saved file and
prints what it said. The editor's own checks are the fast feedback; the game
is the answer.

One of those three is reported but **not counted**. Despite its name,
`--loop-test` is `mine01`'s regression test rather than a general playability
check: `src/game/loop_test.cpp` asserts the level has exactly three keyed doors
and a gunner. Run against anything else it neither skips (it skips only when
there are no doors or no exit) nor passes, so its verdict says nothing about
whether the level is any good. Counting it would train the author to ignore the
one check that catches a mine you cannot finish, so the panel shows it under an
"advisory" note and leaves it out of the headline.

### Two checks the game does not make

Both are warnings, both marked *editor only* in the panel:

* **Overlapping segments.** Nothing in the C++ checks that two segments occupy
  disjoint space. When they overlap, `findSegment` returns whichever it tests
  first and a ship flying there appears to jump between rooms.
* **Unreachable segments.** A segment no portal walk from the player start can
  reach is either dead data or a room nobody will ever see. The game loads it
  and says nothing.

## Two differences from `saveLevel()`

The writer matches `saveLevel()` in `src/game/level.cpp` field for field, with
two deliberate exceptions — both of which make its output a better file than
the C++ writer's, and neither of which changes what loads:

* **`comment` is preserved.** `saveLevel` drops it, so the prose at the top of
  `mine01.json` disappears the first time anyone presses Save in the in-game
  light editor.
* **A light's `segment` is written.** `saveLevel` omits it and lets the loader
  search for it again.

It also keeps each object's key order and omits fields that are at their
default, because the shipped files do. The result is that opening a level and
saving it is a no-op — `git diff` is empty, byte for byte. The C++ writer
cannot manage that: `nlohmann::json` sorts keys alphabetically, and a `float`
widened to a `double` on the way out turns `0.86` into `0.8600000143051147`.

## Vendored

`vendor/three.module.min.js` and `vendor/OrbitControls.js` are three.js
r169, MIT, from the npm tarball; `vendor/LICENSE.three` is its licence. The
minified build is vendored rather than the readable one because the repository
working tree is about a megabyte and the unminified build is 1.3 MB on its own.
