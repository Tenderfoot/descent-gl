# The level editor

A browser tool for authoring `assets/levels/*.json`. It is not part of the
game build: no CMake target, no vcpkg entry, no node toolchain. Three.js is
vendored in `vendor/` and everything else is plain ES modules served straight
off disk.

```
tools/editor/serve.py          # prints the URL it bound to
```

It listens on 8137, or the next free port after it, and prints which. Not 8080:
that one is already taken on a lot of machines — llama.cpp's web UI and half the
dev servers ever written default to it — and the failure is nasty, because the
editor does not start, the browser shows whatever *does* own 8080, and it looks
like the editor is broken rather than absent. Pass `--port` to name one
yourself, and it will fail on that port rather than quietly serving somewhere
you are not looking.

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

E0–E5 are done: the editor views, validates, round-trips, edits geometry,
places entities, and creates a level from nothing. Selections hold more than one
thing, and the gizmo moves, turns and scales the whole of one.

## Making a level (E5)

**New…** writes a starter level into `assets/levels/` and opens it: one
segment, eight vertices, six walls, a player start inside it, and one light so
the room is not simply black. Everything else is grown from it with Extrude.

It is deliberately not a template with rooms in it. A starter level that
guesses at a layout is a layout the author has to delete first.

**Generate…** grows a whole mine instead: rooms, the ways round between them,
lights, robots, a key chain, a reactor and the way out. It exists because the
shipped content is one complete level and seven fragments, and "another mine to
fly" should not cost an evening of extruding.

It drives the same `extrudeSide` and `joinSides` the buttons do, so a generated
mine is built the way a hand-built one is and there is no second definition of
what a level is. Three things it settles by construction rather than by luck:

- **Ways round are exact.** Rooms are grown on a lattice, and a room placed
  where it touches one existing room keeps the graph a tree while one placed
  touching two adds exactly one cycle. So the setting is a count, not a hope.
- **Every key can be reached before the door it opens.** Locked doors sit on
  edges of the spanning tree, which split it in two exactly, so "before this
  door" is a set rather than a guess. Locking a way round would prove nothing —
  you could go round.
- **Rock and built are different shapes, not just different tiles.** Built
  stretches stay on the lattice and take the panel and grate tiles; rock
  stretches are pushed out of true and take the rock tile. A vertex moves only
  when every room sharing it is rock, so the boundary between a corridor and a
  cave stays square on the corridor's side.

Each roll is checked against the editor's own validator and thrown away if it
objects, so the file that lands has already passed the rules the game's loader
enforces. **Verify** afterwards runs the game itself, which is the only thing
that can judge the rest.

The seed a roll reports is the one that produced the mine you are looking at —
when earlier rolls were rejected it is not the number you typed. Type it back
in to get the same mine on the first roll. It stays on the dialog as *last
roll* so it survives the next status message.

Rolling again over the same name **replaces** it, after asking. That is the
loop the button is for, and it is why an existing name is a question here
rather than the refusal **New…** gives: a mine you have discarded should not
outlive you discarding it. Generated levels are validated at every game start
and listed in the player's own level menu, so a session of re-rolls that left
`Generated7` behind would cost something every launch.

A name that was **not** made by the generator gets a blunter question, because
the field is one typo away from `mine01` and a file write cannot be undone from
here.

**Top** (or `T`) is the automap's view — straight down, orthographic, rotation
locked. It is the view that makes "this corridor doubles back over that room"
obvious, which is why the game's automap uses it.

## Editing (E3)

Three selection modes — **1** segment, **2** side, **3** vertex.

**Shift+click adds to the selection**, and shift+clicking something already in
it takes it back out. A shift+click that misses hits nothing rather than
clearing the group, because losing eight vertices to a near miss is the one
thing a multi-selection must not do. Escape clears it.

The gizmo then acts on the whole selection, about its centre: **W** move, **E**
turn, **R** scale, or the buttons under the mode row. Rotate and scale are for
groups — rotating one vertex about itself is a no-op — with one exception: a
single piece of scenery has a facing of its own, so **E** turns it, which is the
only way to aim one without editing the file.

A selection holds either geometry or entities, never both. A vertex and a light
are moved by different functions, and a group holding both would have to answer
what rotating it means; shift+clicking across that line replaces the selection
instead of mixing it. Entity kinds do mix — moving a light and the pickup beside
it together is the point.

Two shortcuts for the selections worth having: with a segment selected,
**Select its 8 vertices**; with a side selected, **Select its 4 vertices**. Both
switch to vertex mode with the corners already picked, which is how a room gets
widened or a wall angled without shift+clicking eight blobs one at a time. The
side version is offered for a portal too: the four vertices a portal shares are
the ones both segments deform through.

A group rotate turns each member's own facing as well as carrying it around the
pivot — a robot's orientation, a spot's aim, a lamp's swing axis — because a
cluster rotated ninety degrees and still facing the way it was placed reads as a
bug. A group scale moves positions only: it does not scale a model, a light's
radius or a cord.

Scale cannot go through zero. Nothing in either validator has a winding rule, so
a segment mirrored by a negative scale references the same vertices in the same
order, passes every structural check, and renders with its normals pointing out
of the mine — a room lit from nowhere that the ship falls through. The handle
cannot express it.

A portal is clickable in side and entity mode, which is the only way one can
be: a portal has no wall triangles of its own, so the translucent quad drawn
for it is the whole of it, and a click resolves against that quad. It selects
the end it was clicked from — fly into a room (double-click) and click its
doorway, and the selection is that room's side of it. In segment mode a click
still passes through the opening to the room beyond, because that is how
double-click flies from room to room. Unticking **Portals** under Show hands
those clicks back to whatever is behind them.

The same now holds for the things inside a mine: a hidden layer is not
pickable, so unticking **Moving lights** is how an author reaches the wall
behind a lamp's swept arc. That arc, and a ball's cage and cord, already carried
a pick record, but the group they are drawn in was not in the candidate list — so
a floating light and a swinging lamp could be selected only from the panel, which
is no use to a shift+click meant to gather a group. They are clickable now.

| Tool | |
| --- | --- |
| **Extrude** | Grows a new segment out of a wall. The wall's four vertices become four of the new segment's, *by index*, so the portal is exact by construction. Depth defaults to the depth of the segment it grew from. |
| **Join** | Makes two walls that are already in the same place into one portal, by rewriting one segment's four vertex indices to the other's. It does not move geometry: if the corners are not already coincident it refuses and says how far off they are. |
| **Split portal** | Turns a portal back into two walls. |
| **Delete segment** | Seals the neighbours, reindexes the graph, the doors and every entity, and takes the lights, pickups and objects inside it with it. The player start and the exit are not furniture and are left pointing at nothing, loudly. |
| **Texture** | Per wall, from the set in `assets/textures/tiles.json`, fetched at startup via `/api/textures`. A tile added to the manifest appears in the picker with no change here. |
| **Emits** | Per wall, how brightly the side glows on its own, on top of the light reaching it. Shown in the view as a self-lit material. An emitting side casts nothing, so where the room has no light of its own the panel offers to drop one at the side's middle. |
| **Vertex drag** | Moves the selected vertices; every segment referencing one deforms together, which is the shared vertex array doing its job. Optional grid snap, off by default. |

**Grid snap** works on the position of a single point and on the *step* a group
takes. Rounding every member of a group to the grid would put distinct vertices
on the same grid point and collapse the shape being moved, so a group keeps its
internal offsets exactly and only its displacement is snapped. It applies to
moving, not to turning or scaling.

Mode **4** places the things inside a mine: lights (radius, intensity, colour,
point or spot), robots and scenery from `assets/models/` drawn as their real
meshes, pickups including keys, the exit trigger, doors on portals, and the two
kinds that move once the level is running. Drag the gizmo to move any of them,
or shift+click several and move them together.

**Floating lights** are a light in a ball: shoot one in the game and it bounces
around the room. Mass divides every impulse it takes, bounce is how much of a
wall it keeps, and drift is how fast it wanders when nothing has hit it.

**Swinging lights** hang from the ceiling on a cord. What the author places and
drags is the **anchor**; the lamp is a cord's length below it. The file calls
that point `anchor`, and inside the editor it is an entity's ordinary
`position`, so selection, dragging, segment resolution and validation are
written once rather than once per kind — `parseLevel` and `serializeLevel` are
the only two places the name changes.

A lamp is hard to author blind: a cord length and an amplitude in radians say
nothing about whether it will sweep into a pillar. So the viewport draws the
whole arc, the cord at both extremes, and the spot direction, and the panel
takes the swing in degrees and prints the period in seconds. "Add swinging
light" hangs it from the middle of the selected segment's ceiling on a cord
short enough to clear the floor, because the game is fatal on one that does
not.

Both kinds are capped in the simulation at eight per level. Past that the game
warns and ignores the rest, so the editor refuses the ninth rather than letting
you place something that will silently not be there.

The rule all of it protects is that an entity's cached `segment` says which
segment actually contains it. The loader recomputes a `segment` that is missing
or out of range but **trusts one that is merely wrong** — so a light dragged
across a portal without re-resolving lights the wrong room and nothing anywhere
complains. Everything that moves something re-resolves it, against the same
`findSegment` the loader uses.

A door can only go on a portal, and the key colours offered are only the ones
the level actually has a key for: a red door in a mine with no red key is a
mine that cannot be finished. So: place the key first, then click the portal in
mode **4** and pick a colour. A portal that already carries a door offers to
delete it instead — a portal cannot have two.

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
| `src/entities.js` | lights, robots, pickups, doors, the exit, the start, the moving lights |
| `src/create.js` | the starter level, and what a level may be called |
| `src/generate.js` | growing a whole mine, and the re-roll loop that checks it |
| `src/obj.js` | just enough OBJ to draw the shipped models |
| `src/history.js` | undo and redo, by snapshot |
| `src/transform.js` | the group transform, as plain arrays so it needs no three.js |
| `src/view.js` | the only module that imports three.js |
| `src/main.js` | the application |
| `serve.py` | static files, level read/write, and the Verify endpoint |
| `tests/conformance.mjs` | the editor's validator against the game's |
| `tests/ops.mjs` | the structural edits, checked for shape and not just legality |
| `tests/entities.mjs` | entity placement, and the segment-resolution rule |
| `tests/portals.mjs` | what a click on a portal resolves to |
| `tests/create.mjs` | the starter level, and that it can be grown |
| `tests/transform.mjs` | transforming more than one thing at once |
| `tests/generate.mjs` | the generator's promises: a seed repeats, the counts are exact, the keys come first |
| `testdata/base.json` | the three-segment level every broken variant is made from |

## Verification

```
node tools/editor/tests/conformance.mjs   # the validator, against the game's
node tools/editor/tests/ops.mjs           # the structural edits
node tools/editor/tests/entities.mjs      # placement and segment resolution
node tools/editor/tests/create.mjs        # the starter level
node tools/editor/tests/portals.mjs       # what a click on a portal resolves to
node tools/editor/tests/transform.mjs    # transforming more than one thing at once
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

The panel reports each check as a row rather than as a wall of text, because
two very different things hide behind a non-zero exit and the difference is the
one an author needs:

* **cannot load** — the game aborted. The level is broken and nothing else
  matters until it is fixed.
* **N note(s)** — the level loaded and the check had objections. Some of those
  mean the level is *too small to exercise what the check tests* rather than
  that it is wrong: a one-room mine trips "no segment is dark" and "portal
  culling is doing nothing" no matter how correct it is.
* **advisory** — the check was written for another level. `--loop-test` is
  `mine01`'s regression test rather than a general playability check:
  `src/game/loop_test.cpp` asserts the level has exactly three keyed doors and
  a gunner, and it skips only when there are no doors or no exit, so an
  editor-built level is neither skipped nor passing. Counting it would train
  the author to ignore the one check that catches a mine you cannot finish.

Only "cannot load" counts against the headline. The full output is one click
away underneath.

### Three shapes a generated mine must not have

Found by generating mines and running the game over them, and all three pass
every structural rule while being wrong:

- **Two rooms meeting only along an edge.** Diagonal neighbours with nothing
  between them are not two rooms with rock between them — their four walls all
  run into one line, and the surface has four triangles on an edge that may
  only have two. `deleteSegment` carries the same warning for the same shape.
- **A ring of rooms around an empty cell.** That is a doughnut, so the wall
  surface is a torus and `--world-test` rejects it: Euler characteristic 0, not
  2. A way round made of a *filled* square of four rooms is the same graph
  cycle with no handle, and passes. `FigureEight` and `MineEntrance` both ring
  something and both fail this check today.
- **A room that is a perfect cube.** Its centre sits exactly on the diagonal
  through four of its edges and both corner diagonals, and the game's raycast
  uses Möller–Trumbore with no edge slack — deliberately, so a wall never
  over-reports. A ray down that diagonal can miss every triangle at the corner
  and report a ray leaving a closed mine. Neither hand-built level contains a
  perfect cube; a generator emitting nothing else tripped it on the first ray.
  The generator's lattice pitch is `20 x 17 x 23` for this reason alone.

### Three checks the game does not make

All three are warnings, all marked *editor only* in the panel:

* **Overlapping segments.** Nothing in the C++ checks that two segments occupy
  disjoint space. When they overlap, `findSegment` returns whichever it tests
  first and a ship flying there appears to jump between rooms.
* **Unreachable segments.** A segment no portal walk from the player start can
  reach is either dead data or a room nobody will ever see. The game loads it
  and says nothing.
* **A lamp that swings out of the room.** `loadLevel` checks a swinging light's
  cord straight down and is fatal on one that reaches outside the mine, which
  the editor matches as an error. It does not check the ends of the swing, so a
  lamp that hangs clear but leaves the room at the extremes of its arc loads
  and then lights through rock.

## Three differences from `saveLevel()`

The writer matches `saveLevel()` in `src/game/level.cpp` field for field, with
three deliberate exceptions — all of which make its output a better file than
the C++ writer's, and none of which changes what loads:

* **`comment` is preserved.** `saveLevel` drops it, so the prose at the top of
  `mine01.json` disappears the first time anyone presses Save in the in-game
  light editor.
* **A light's `segment` is written.** `saveLevel` omits it and lets the loader
  search for it again.
* **A side's `door` is not written.** `saveLevel` writes it; it is derived.
  `loadLevel` rewires every side from the `doors` array whatever the file says,
  so writing it puts a second copy of the door graph in the file for the two to
  disagree about. The shipped levels carry it on no side at all, which is also
  why the editor has to do that rewiring at load: without it a portal with a
  door on it read as empty, and the editor offered to add a second one.

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
