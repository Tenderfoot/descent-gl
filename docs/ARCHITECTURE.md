# Architecture

Living document. Each milestone adds its section. Read this before changing
anything; it records decisions that are expensive to reverse and the reasons
they were made, which are not recoverable from the code alone.

---

## The three permanent bets

These are leaned on by every milestone from M2 onward. Do not compromise them
for convenience.

1. **Levels are segment graphs.** A mine is a graph of deformed cubes, each
   with 6 sides that are either textured walls or portals into a neighbour.
   One structure delivers visibility (portal walk), collision (6 plane tests),
   pathfinding (BFS), and light culling (per-light segment sets).
2. **No ambient light.** A surface no light reaches is `(0,0,0)`. Rendering is
   a depth prepass followed by one additive pass per light. The full blacks are
   the absence of a pass, not a colour grade.
3. **Stencil shadow volumes.** Silhouette edges extruded to infinity, counted
   in the stencil buffer with depth-fail. Pixel-exact hard edges, no acne, no
   shadow-map resolution.

Status: all three are in. Bets 1 and 2 landed in M2 — see "The level format"
and "The lighting frame". Bet 3 landed in M3 — see "Stencil shadow volumes".

---

## Why OpenGL, not Vulkan

The repository is named `descent-vk` and the plan was originally written for
Vulkan 1.3. It was changed to OpenGL 4.6 core before M0 was implemented.

Vulkan's advantages — explicit multithreaded command recording, fine-grained
memory control, low driver overhead at high draw-call counts — do not apply at
this project's scale: a few thousand low-poly triangles and on the order of a
dozen lights. What it does cost is a large, permanent plumbing tax on every
milestone.

Stencil shadow volumes are also native to this API. Doom 3 is an OpenGL
renderer, and its source is the reference to consult when something looks
wrong; it maps onto GL directly and onto Vulkan only loosely.

Everything the plan needs is core GL 4.6: two-sided stencil, MSAA,
`glClipControl` for reversed-Z, texture arrays, timestamp queries, and
`KHR_debug` for validation-style error reporting.

What was given up: the `VK_KHR_ray_query` fallback for M3's shadows. There is
no GL equivalent. This matters less than it sounds, because stencil volumes are
lower-risk in GL to begin with.

The directory name was left alone rather than churn paths and git history.

---

## Frame structure

```
bind MSAA target
  clear colour (black) + depth + stencil
  depth prepass                        (all visible geometry, depth only)
  for each light reaching the visible set:
      clear stencil
      shadow volume pass               (no colour write, depth-fail counting,
                                        GL_GREATER)
      additive lighting pass           (blend ONE,ONE; depth EQUAL;
                                        stencil == 0; this light's segments)
  unlit pass                           (emissive and debug; M5 bolts and
                                        flares, M7 HUD, M8 automap)
resolve (blit) to the default framebuffer
ImGui on the default framebuffer
swap
```

ImGui is drawn *after* the resolve, not into the MSAA target. Its geometry is
axis-aligned text and rectangles that gain nothing from multisampling, and
keeping it out means the resolve carries only scene colour.

Silhouettes for every light are extracted on the CPU *before* any drawing, and
all of a frame's volumes go into one buffer that is uploaded once. The per-light
loop then draws a range out of it.

### The depth prepass and shader invariance

The lighting passes test `GL_EQUAL` against the depth the prepass wrote. That
only works if both shaders compute *bit-identical* positions, so
`depth.vert`, `wall.vert` and `unlit.vert` all declare `invariant gl_Position`
and spell the expression exactly the same way:

```glsl
gl_Position = uProj * uView * uModel * vec4(aPos, 1.0);
```

This is not a style rule. `wall.vert` originally computed the world position
first and reused it — `uProj * uView * (uModel * p)` — which is the same value
in exact arithmetic and a different one in the last bit of a float. `GL_EQUAL`
then rejected scattered fragments and the walls rendered shredded with thin
unlit lines. Nothing about that symptom points at the vertex shader. **If you
add a pass that draws into this depth, copy the expression verbatim.**

---

## The level format

`src/game/level.h`. A mine is a graph of deformed cubes. A segment is 8 vertex
indices into one shared level vertex array plus 6 sides, each of which is
either a textured wall or a portal into a neighbour.

Sides are always in the order **left, top, right, bottom, back, front**, and
`kSideVertices` maps each to its four corners, wound counter-clockwise *seen
from inside the segment*. The mine is a space you are inside, so a wall's front
face and its normal both point inward.

Sharing the vertex array is what makes portals exact: two segments joined
through a portal reference the *same four vertex indices*, so there is no seam
to leak light through and no gap to fall through, by construction rather than
by tolerance.

### Sides are not planar, and are split at load

A deformed cube's side is a general quad. At load each is split into two
triangles by whichever diagonal leaves the smaller crease — the pair of
triangle normals with the larger dot product. **Every later system uses these
triangles and never the quad**: rendering, collision, and M3's shadow volumes.
In `test01`, 42 of 84 sides take the alternate diagonal, so this is not a
theoretical case.

### Validation is fatal

`loadLevel` refuses a file that breaks a structural rule, naming the segment
and side. The rules: vertex indices exist, a portal names a segment that
exists and is not itself, the neighbour names this segment back, and **the two
sides of a portal reference the same four vertices**. That last one is the one
worth having. Checking only that the neighbour points back would accept two
segments joined through different faces, which walks and renders perfectly
right up until a light leaks through the seam — a symptom that looks like a
renderer bug and is not.

### UVs

A side with no authored UVs gets a planar mapping at a fixed world size per
tile (`kWorldUnitsPerTile`), so texel density is the same on a corridor wall
and on the room's raised ceiling. A flat 0..1 per side stretches one 256-pixel
tile across a 20-unit wall.

---

## Authoring: the level editor (E0-E2)

`tools/editor/`. A browser tool that reads and writes `assets/levels/*.json`.
It is not part of the game build — no CMake target, no vcpkg entry, no node
toolchain — and three.js is vendored beside it.

`mine01` is 124 vertices and 34 segments written by hand, which is about the
ceiling for a text editor. The reason a tool is needed rather than more
patience is the format's central rule: two segments joined through a portal
reference **the same four vertex indices**, not four coincident positions.
Nothing about that is visible while typing JSON, and getting it wrong produces
a light leak that looks like a renderer bug.

So the editor is segment-graph-native. It knows about segments, portals and
the shared vertex array; from E3 the primitive operation is *extrude a side
into a new segment*, which creates the reciprocal portal and shares the four
vertices by construction. There is no free vertex-soup mode.

### It draws the game's triangulation, not its own

`tools/editor/src/geometry.js` is a port of `splitSides()`. A side is a general
quad, split at load by whichever diagonal leaves the smaller crease, and
**every later system uses those triangles and never the quad**. An editor that
drew the quads, or split them the other way, would be showing a mine subtly
different from the one the game builds, with no error anywhere to say so.

That the port is faithful is checked by a number rather than by reading it:
the editor must reproduce the loader's own log line, *"81 of 204 sides took the
alternate diagonal"*, exactly, for both shipped levels. That number moves if
the rule is ported wrong.

### Two validators, and the conformance run

`tools/editor/src/validate.js` reimplements `validate()`, the door and entity
resolution that follows it in `loadLevel()`, and `asset_check.cpp`'s
closed-surface check. Two implementations of one format drift, and the way
anyone finds out is by authoring a level the editor called fine and the game
aborts on.

So `tools/editor/tests/conformance.mjs` mutates a three-segment base level one
rule at a time and asks both: the editor must reject it, the game must abort
on it, and the two must name the same segment and side. Fifteen cases, one per
rule. Compiling the C++ validator to WASM was the alternative and was rejected:
a whole toolchain, for seventy lines.

Two checks exist only in the editor, as warnings, because the game makes
neither and both produce behaviour that looks like a bug elsewhere:
**overlapping segments** (`findSegment` answers with whichever it tests first,
so a ship jumps between rooms) and **unreachable segments** (dead data, or a
room nobody can enter). The conformance run asserts the *inverse* for these —
the editor warns and the game loads the level silently — which is what keeps
them honest about being editor-only.

### Saving is a no-op

Opening a shipped level and pressing Save produces the same bytes: `git diff`
is empty. This is proved before any editing feature exists, because a round
trip that is only *nearly* faithful turns every later save into an
unreviewable diff.

`saveLevel()` cannot manage it. `nlohmann::json` sorts object keys
alphabetically, and a `float` widened to `double` on the way out turns `0.86`
into `0.8600000143051147`. The editor's writer keeps each object's source key
order, omits fields sitting at their default, and formats numbers the way the
files do — floats with a `.0`, indices bare. It also fixes two things
`saveLevel` gets wrong: it **preserves `comment`**, which `saveLevel` drops (so
the prose atop `mine01.json` survives a save), and it **writes a light's
`segment`** rather than making the loader search for it again. It also **omits
a side's `door`**, which `saveLevel` writes: that field is derived, `loadLevel`
rewires every side from the `doors` array whatever the file holds, and a second
copy of the door graph in the file is only something for the two to disagree
about.

### Picking a portal

A portal is not a surface. `splitSides()` emits triangles only for sides with
no neighbour, so a portal has no geometry in the game and none in the editor
either — the translucent quad in the viewport is drawn from the side's four
corners and is the whole of it. That quad is what a click has to resolve
against, and until it carried a per-triangle record the way the wall meshes do,
a ray aimed at a doorway passed through the opening and selected the far wall
of the next room. Every portal tool was therefore unreachable by clicking: a
door goes on a portal, and so does Split portal.

`portalFaces()` in `geometry.js` is the description that was missing, and it
carries **both** ends of each portal, because a portal is a side of two
segments and a click has to name one of them. `portalFacing()` picks the end
the click came from, by the sign of the look direction against the face normal
— the same convention as `sd.normal`, which points into the segment that owns
the side. Both are in `geometry.js` rather than `view.js` so that the part that
can be wrong is testable under node with no browser: `tests/portals.mjs`.

Segment mode still clicks straight through a portal to the room beyond, which
is what makes double-click fly from room to room, so portals are pickable only
in the two modes where a *face* is what the click is for.

Resolving doors at load belongs to the same fix. `loadLevel` wires `sides[].door`
from the `doors` array on both ends of each portal (`level.cpp`), and the
editor's `parseLevel` did not — a gap with no symptom while portals could not
be clicked, and a duplicate door the moment they could, because a portal whose
door the editor could not see is a portal it offers to add a door to.

### Transforming more than one thing

A selection holds a list, and the gizmo moves, turns and scales the whole of it
about its centre. Two rules make that safe, and both are about where the maths
lives and what it is applied to.

`tools/editor/src/transform.js` speaks plain arrays and imports nothing.
three.js lives only in `view.js`, so the part of a group transform that can be
wrong — the pivot, the quaternion, the composition — is testable under node with
no browser, the same reason `portalFaces()` is in `geometry.js`.
`tests/transform.mjs` is that test.

Every frame of a drag applies the gizmo's pose to the positions recorded when
the drag **started**, never to the ones it last produced. `TransformControls`
reports absolute state — the whole drag so far — so feeding its output back in
compounds, sixty times a second, and a group comes out of a rotation slightly
deformed by the act of having been moved. The same rule is why `turnEntity()`
takes the facing to rotate *from*: composing an absolute rotation onto an
object's current orientation once per frame spins it off into the rock.

Two consequences worth writing down. A group snaps the *step* it takes rather
than each member's position, because rounding every vertex of a group to the
grid puts distinct vertices on one grid point and collapses the shape. And scale
cannot reach zero or go negative: neither validator has a winding rule, so a
segment mirrored by a scale handle references the same vertices in the same
order, passes every structural check, and renders with its normals pointing out
of the mine. `MIN_SCALE` makes the drag unable to express it, which is the only
place that check exists.

### The server, and Verify

`tools/editor/serve.py` is loopback-only and does the three things a browser
cannot: list the levels, read one, write one back. It also exposes **Verify**,
which runs the real binary — `--world-test`, `--collision-test`, `--loop-test`
— over the saved file and prints what it said into the panel. The editor's own
checks are the fast feedback; the game is the answer. Nothing new was invented
to verify levels, because the headless flags already do it.

The level name in a request is matched against a strict pattern and joined to
`assets/levels/` rather than taken as a path, static files are resolved and
then confirmed to still be inside `tools/editor/`, and a save parses the body
before replacing the file — unreadable JSON in `assets/levels/` makes every
later game start fatal, editor or not.

### Editing, and the one algorithm that matters (E3)

The primitive is **extrude**: a wall grows into a new segment whose matching
face reuses the wall's four vertex *indices*. The portal is exact because it
was never built any other way.

Which of the new segment's eight corner slots takes which existing vertex is
the whole of the problem, and it is the only place in the editor where a bug
produces a level that passes every structural rule and is still wrong — a
twisted segment. Every rule in the format is about indices; a twist is about
which index went in which slot. So the correspondence is **derived** from the
cube's corner coordinates (the permutation that reflects the cube through that
side's plane) rather than written down: a hand-built table is right for the
side it was checked on and quietly twisted for the other five. `tests/ops.mjs`
extrudes in all six directions and checks the result is a *box* — eight
distinct corners, no degenerate edge, every face planar — not merely a legal
one.

**Join** is the same rule from the other end. Two walls already in the same
place become a portal by rewriting one segment's four indices to the other's,
across the whole level, and dropping what that makes redundant. It never moves
geometry to close a gap: a portal made by nudging vertices until they coincide
is exactly the file that validates, renders, walks, and leaks light. If the
corners are not already coincident it refuses and says how far off they are.

Two consequences worth knowing, both of which the editor reports rather than
hides:

- Extruding into a space where a vertex already exists **reuses** it, so two
  extrudes that meet already share their four corners and the join between
  them only has to set the portals. In between, the level has two walls in the
  same place — an over-shared surface — and the closure check is what tells
  the author to join them.
- Deleting a segment from the middle of a cluster can leave four faces meeting
  along one edge. The shadow volumes cannot use that, and no cleverness in the
  delete fixes it, because the geometry genuinely pinches. The closure check
  reports it, Save refuses, and undo is one keystroke away.

Delete also takes the lights, pickups and objects inside the segment with it —
keeping the lamp floating in the rock the room became is not what anyone means
— but not the player start or the exit, which are the level rather than
furniture and are left pointing at nothing, loudly.

Every edit goes through one function that records undo, applies the operation,
revalidates and redraws. Nothing calls the operations directly, so there is no
path that changes a level and forgets to re-check it. Undo is by snapshot
through the writer and reader, which means an undone level is byte-identical
to one loaded from the file — and that a bug in the writer surfaces here
rather than a week later in a diff.

### Entities, and the field the loader trusts (E4)

Lights, robots and scenery, pickups, doors, the exit and the player start.
Objects are drawn as their real meshes — `tools/editor/src/obj.js` is just
enough OBJ to get a silhouette, checked against the triangle counts
`--validate-assets` prints, because placing a robot as a grey box is placing it
and hoping.

The rule all of it protects is one field. Every entity caches which segment
contains it, and `loadLevel` recomputes that field when it is missing or out of
range — but **trusts it when it is merely wrong**. So an entity dragged across
a portal without re-resolving produces a light that lights the wrong room, or a
pickup in a segment the player never enters, and no validator anywhere
complains because nothing is structurally invalid. Everything in the editor
that moves an entity re-resolves the field against the same `findSegment` the
loader uses, and the tests assert the answer matches `findSegment` rather than
matching a hard-coded index.

Two things the editor refuses that the game does not check: a door anywhere but
on a portal (the game is fatal on this, so refusing is just saying it earlier),
and a door keyed to a colour the level has no key for — a red door in a mine
with no red key is a mine that cannot be finished, and nothing checks for it.

### `--loop-test` is not a general oracle

Worth recording, because the name says otherwise and the editor's Verify button
originally believed it. `src/game/loop_test.cpp` asserts the level has exactly
three keyed doors and a gunner: it is `mine01`'s regression test, not a check
of whether an arbitrary level can be played. It skips only when a level has no
doors or no exit, so an editor-built level with one door and no gunner is
neither skipped nor passing.

The server therefore marks a check with the level it was written for, and the
panel reports that check under an advisory note without counting it. Folding
its verdict into every level's result would train the author to ignore the one
check that catches a mine you cannot finish. What *is* general in its output is
still asserted in the editor's own tests: that the key the editor placed gets
seen, collected, and has an effect.

### Starting from nothing (E5)

A tool that can only edit the two levels already in the repository is not an
authoring tool. **New** writes a starter level — one segment, eight vertices,
six walls, a start and a light — and opens it; everything else is grown with
extrude. It is deliberately not a template with rooms in it, because a guessed
layout is a layout the author deletes first.

The starter is built by handing a plain object to the same reader a file goes
through, so it carries the same derived data and cannot drift from what a
loaded level is. `tests/create.mjs` asserts it is extrudable from all six
sides, not merely valid: a starter that validates and cannot be grown is no
use.

### Reading the game's verdict

The Verify panel reports each check as a row, because two very different things
hide behind a non-zero exit:

- **The game aborted.** The level is broken; nothing else matters.
- **The game loaded it and objected.** Some objections mean the level is *too
  small to exercise what the check tests*. A correct one-room mine trips "no
  segment is dark" and "portal culling is doing nothing" from `--world-test`,
  and a correct five-room mine trips `--loop-test`'s assertions about
  `mine01`'s three keyed doors.

Only an abort counts against the headline; everything else is shown and
explained. This matters more than it sounds: a panel that cried failure at
every correct small level would teach the author to ignore the checks, and one
of those checks is the only thing that catches a mine you cannot finish.

### What it is not

Not the renderer. No shadows, no falloff, no reversed-Z, and the colours are
flat and deliberately unlike the game's: this is a diagram, and reading it as
a screenshot would be a mistake. A mine seen from outside is a closed hull, so
walls are `DoubleSide` and an x-ray toggle drops them to a shell; double-click
flies the camera into a segment. "How does this look" is answered by
`--segment N --no-ui --screenshot`.

The in-game ImGui light editor stays. Tweaking a light under real shadowed
lighting is not something a browser can do; structure is what the editor owns.

---

## Portal visibility

`src/game/visibility.h`. From the segment the camera is in, recurse through
portals, projecting each to a screen-space rectangle and intersecting it with
the rectangle it was seen through. Empty rectangle, nothing beyond it is
visible.

**Portals are clipped against the near plane before projection.** A portal
beside the camera straddles it, and projecting a corner behind the eye divides
by a negative `w`, putting it on the wrong side of the screen and producing a
mirrored, enormous rectangle. The first version fell back to the parent window
in that case — conservative, and useless: at a segment's centre nearly every
portal straddles, so every portal inherited the whole screen and the walk
reported **all 14 segments visible from anywhere in the mine**. With a proper
Sutherland-Hodgman clip against `w > epsilon` (four corners and one plane give
at most five, so no allocation), the worst case is 10 of 14 and the start
segment sees 4.

The rectangle is an axis-aligned bound, not the portal's exact shape, so the
walk over-reports slightly around bends. That is the intended trade: it can
only ever draw too much, never too little, and too little is what makes holes.

Each segment accumulates a bounding union of the screen windows through which
it has been reached. A newly exposed window triggers another traversal, even
when the segment was already reached at the same or a shallower graph depth.
A shorter path also triggers traversal to restore the remaining depth budget.
Graph depth alone cannot establish that all visible exits were explored.
Coverage is reset every frame, including when a door opens or closes.

The ImGui "freeze visible set" toggle stops the walk being rebuilt while the
camera keeps moving, so the cull can be inspected from outside it. Without it
the one thing you cannot see is what the cull decided.

---

## The lighting frame

**There is no ambient term and there will not be one.** A surface no light
reaches keeps the clear colour, which is black. The darkness is the absence of
a pass, not a grade, and it is what turns the headlight and flares of later
milestones into mechanics rather than decoration.

A light carries position, radius, colour, intensity, type, and a precomputed
**segment set**: flood-filled from the light's own segment, stopped at any
portal whose nearest point is farther away than the radius. Distance is
measured to the nearest point of the portal itself, not to the neighbour's
centre — a long segment whose centre is out of range can still have a doorway
well inside it. A light is only ever considered against segments in its set,
intersected with what the portal walk says is on screen, so a light in a room
you cannot see does no work at all.

The set is rebuilt whenever a light moves or its radius changes. A stale set is
how a light ends up lighting a room through a wall.

### Falloff

```glsl
float t = dist / uLightRadius;
float window = clamp(1.0 - t*t*t*t, 0.0, 1.0);
float attenuation = window * window / (1.0 + 12.0 * t * t);
```

The window makes the light reach exactly zero **at** the radius rather than at
infinity, which is what lets the segment set be an exact bound instead of an
approximation. That matters: a light still faintly lit at the edge of its set
would visibly pop as the set cut it off.

The inverse-square term is in `t = dist/radius`, not in world units.
Absolute inverse square is only meaningful if intensity is in absolute units
too, and at this project's scale — 20-unit segments — that means authoring
lights in the hundreds and still getting a pinprick at the source with
blackness two metres away. The first attempt did exactly that and the mine was
unreadable. Radius-relative falloff gives one knob that means what it looks
like it means: radius sets both reach and shape, intensity stays a small
readable number (2–4 in `test01`).

### Asking how much light is on a point

The falloff above is the GPU's answer, and it only ever exists as pixels. The
same question about a **point in the air** — "is the player standing somewhere
lit?" — is answered on the CPU by `src/sim/lighting.h`, with no GL, no window
and no allocation, so the simulation and the headless tests can both ask it.

`lightFalloff` is the falloff above transcribed into C++, and it is now the
only CPU copy: `--world-test`'s brightness report calls it rather than carrying
its own. `--light-test` checks it against a third transcription taken straight
from the text of `wall.frag`, because two copies of a formula in two languages
is exactly the pair that drifts.

A query is a point, a segment, and optionally a body radius:

```cpp
sim::IlluminationQuery q;
q.point = shipPosition;
q.segment = shipSegment;
q.bodyRadius = body.radius;      // 0 asks about the point alone

sim::Illumination lit;
sim::addIllumination(lit, q, level, level.lights, &doors);
sim::addIllumination(lit, q, level, dynamicLights, &doors);
if (lit.dark()) { /* nobody can see you */ }
```

Per light it is a segment-set test, then the falloff and the spot cone, then
one `sim::raycast` from the point to the light. The segment set does the real
culling and it is exact, not conservative: the falloff reaches zero at the
radius and the set is flood-filled to that same radius through open portals, so
a light whose set excludes the segment contributes exactly nothing.

Lights are added one list at a time because the level's lights and the frame's
dynamic ones — the headlight, every bolt in flight, every burning flare — live
in different places and neither is a copy of the other.

With a body radius the query samples seven points, the centre and one radius
out along each axis, and reports `litFraction()` alongside the value. A single
point flips the instant a shadow edge crosses it; a body straddling that edge
is the case the fraction exists to describe.

**One deliberate disagreement with the screen** — and, as of M8, one fewer than
there used to be:

1. ~~Only the mine occludes.~~ **Closed in M8.** Pass an `OccluderSet` and
   robots, props, the ship and the level's objects block light here too. The
   old note said closing the gap would mean reaching into the occluder meshes
   behind `render/gl.h`; proxy volumes made that untrue, because `sim` is handed
   finished shapes and never reads an asset. Without a set the query still falls
   back to walls and door faces, which is what every caller did before.
2. **The shadow budget does not apply.** A light past the renderer's cap still
   lights but stops casting, so on screen it shines through walls; here it
   never does. That is the point: a mechanic built on this does not change
   with frame rate.

`--light-test` cross-checks the segment-walk ray against brute-force
ray/triangle over every wall triangle in the level — a genuinely different
method, no portals and no graph — and requires the two verdicts to agree on
every point-light pair.

### Being seen

`playerVisibility` is the rule the AI acts on, and it is not the same thing as
the illumination above.

It gathers the level's lights, then every light the **simulation itself** owns
— bolts in flight, burning flares, the flash off a gun — through `addLooseLight`,
which skips the segment-set cull because these carry no set to cull with.
Falloff plus one shadow ray is the whole answer, and there are only ever a
handful near any one point.

Two things separate it from a plain illumination reading:

- **The headlight counts.** A player flying with the lamp on is carrying a light
  source, and the fact that a forward-facing spot barely illuminates the hull it
  is bolted to does not make its owner hard to spot. `visible` is true whenever
  the headlight is on, whatever is falling on the ship.
- **It is a point query, not a body one.** It runs every simulation step; seven
  sample points would cost seven times as much for coverage the AI does not use,
  and a hull straddling a shadow edge would flicker. Callers wanting coverage
  ask `illuminationAt` directly.

Both are measured: `--light-test` checks that 34 000 visibility queries allocate
nothing, because the stack `Light` it builds per loose light costs nothing only
while its segment vector is left alone.

### Dynamic occluders

`src/sim/occluders.h`. An `OccluderSet` is everything that stands between a
light and a surface, or an eye and its target, and is not the mine: the ship,
every live robot, every floating light, and every solid level object. Rebuilt
every step, fixed-capacity, allocation-free.

**Bodies go in as spheres of their own radius, not their model's bounds.** The
body radius is the authored, tuned number that collision and gunfire already
agree on, and a robot that blocks sight over a different extent than it blocks
bullets is a robot the player cannot aim at. Objects go in as their
`PlacedProxy`, on the same `alive` flags that decide whether they stop a
bullet — shoot a crate away and it stops casting too.

#### Robot sight is where the mechanic lives

This is the correction that mattered most, and the roadmap had it wrong. The
plan was to improve `addIllumination` and call the payoff "pillars become
cover". Robot perception does not work the way that assumed:

```cpp
const bool alreadyOnToThem = robot.awareness > 0.0f;
if (canSee && (visibility.visible || alreadyOnToThem)) {
```

`visibility.visible` — the value the plan was going to improve — is **bypassed
entirely** once a robot is alert. For every robot already on to the player, only
`canSee` matters, and `canSee` was a `raycast` against walls and shut doors with
no level objects in it at all. Improving the light query would have changed
exactly one case: a robot that had never seen the player deciding whether to
notice them.

So the occluders go into that ray first. `--robots-test` measures the result:
awareness **0.00** behind a column, **1.00** on a clear line, and **1.00** with
the column simply not in the set — that last one proving the line is otherwise
clear. An already-alert robot held two seconds behind a column decays from
**1.00 to 0.33**.

#### Nothing shadows itself

`OccluderId` is not bookkeeping; it is the difference between working and
broken. `playerVisibility` samples the ship's own centre, so including the
ship's proxy starts every shadow ray inside it: the player reads unlit against
every light in the mine and nothing ever notices them again. Not a flicker — the
mechanic simply stops. `IlluminationQuery::owner` names the body being asked
about, and `addLooseLight` takes an `emitter`, because a floating light inside
its own housing would otherwise put itself out.

`--light-test` covers both, each with a positive control: with the ship's own
proxy in the set the reading in the brightest room is unchanged at 2.87, and a
robot standing in the same spot drops it to 0.00.

#### The slab test, checked against triangles

`rayProxyDistance` is an analytic slab test. `--light-test` cross-checks it
against the box's own twelve triangles through Möller-Trumbore — the same
routine the mine's walls use, entirely different arithmetic — over 1260 rays
from fourteen directions. They agree on every verdict and to **0.000019 units**
on distance.

That check needs `edgeSlack`, for the reason the parameter was added in the
first place. A ray down a box's corner enters exactly where two faces meet, and
with no slack Möller-Trumbore misses *both* triangles and reports the far side:
on `mine01`'s reactor that is an 18.7 unit error. The slab test has no such
seam, which is why it is the one in the shipping path.

#### Cost

The stated risk for this phase was per-step cost: `playerVisibility` runs 120
times a second and every shadow ray that clears the mine now also walks the set.
Measured with 42 occluders, it is **+4.59 µs per query against an 8333 µs
step** — under 0.06%. The cheap reject is a dot product and a compare against
the occluder's bounding radius, which throws out everything not roughly on the
line before any real intersection work.

#### What this deliberately makes worse

Adding dynamic occluders here while the renderer still drops casters past the
shadow budget means the two answers can disagree in a new way: a light that has
lost its budget slot shines through a robot on screen while this query says it
does not. That is a real regression in the rare case, taken knowingly, because
the common case goes the other way — today the query says "lit" whenever a robot
visually blocks a lamp, and after M8 it agrees with the screen there. A frequent
disagreement traded for a rare one.

---

## Reversed-Z with an infinite far plane

`src/render/camera.h` builds the projection; `Renderer`'s constructor sets the
matching state:

```cpp
glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
glDepthFunc(GL_GEQUAL);
glClearDepth(0.0);
```

Depth runs near = 1.0 to infinity = 0.0. Two reasons, both structural:

- **Precision.** Float precision clusters near zero. Reversed-Z puts the far
  distances there, which is where depth fighting would otherwise appear.
- **Shadow volumes.** M3 extrudes silhouette edges to infinity by emitting
  `w = 0`. Under a conventional finite far plane those vertices clip, and the
  stencil counts go wrong. With an infinite far plane they land exactly on it
  and the counting stays correct.

The matrix maps a view-space point at `z = -zNear` to `ndc.z = 1` and
`z = -infinity` to `ndc.z = 0`:

```
m[0][0] = f / aspect      f = 1 / tan(fovY / 2)
m[1][1] = f
m[2][3] = -1
m[3][2] = zNear
```

**Do not replace this with a conventional projection without revisiting M3.**
Every depth comparison in the project and the correctness of shadow volumes
both depend on it.

---

## Render target

`src/render/target.h`. One offscreen framebuffer:

- colour: `GL_RGBA8` multisampled renderbuffer, 4 samples
- depth-stencil: `GL_DEPTH32F_STENCIL8` multisampled renderbuffer, 4 samples

Resolved to the default framebuffer with `glBlitFramebuffer`.

The stencil aspect is **unused in M0 but must exist and be multisampled.** M3
counts shadow volumes in it, and multisampled stencil is what anti-aliases the
hard shadow edges — the technique's characteristic look with none of a shadow
map's resolution artifacts.

Renderbuffers rather than textures because nothing samples them yet. If a later
milestone needs to read the colour or depth in a shader, switch to
`GL_TEXTURE_2D_MULTISAMPLE`; the rest of the class is unaffected.

`resize()` is idempotent and safe to call every frame; it rebuilds only when
size or sample count actually changes.

---

## Fixed timestep

`src/main.cpp`. Simulation runs at a fixed 120 Hz with an accumulator;
rendering interpolates between the two most recent states with
`dvk::interpolate`.

`SimState` and `RenderState` are deliberately distinct types. Render code
receives a `RenderState` and therefore cannot read or mutate simulation state
by accident.

Details that matter:

- A frame longer than 0.25 s is clamped, so a breakpoint or a dragged window
  does not trigger a burst of catch-up steps.
- Mouse motion accumulates in `Input` until consumed, so a frame with zero sim
  steps does not drop it. The whole frame's motion is applied to the first step
  of that frame; later steps in the same frame see zero, because there is no
  new input to apply.
- While minimized the accumulator is zeroed rather than banked, and the elapsed
  time is excluded from the reported step rate.

The step rate printed at exit is the verification that this is real rather than
cosmetic: it must sit on 120 regardless of frame rate. Measured 119.7–119.8 in
both configurations at frame rates from 200 to 9600 fps.

---

## The ship and the sim/render split (M1)

`src/sim/ship.h` holds `ShipState`: position, orientation, linear velocity and
two angular velocity channels. `src/game/state.h` holds the `SimState` that
wraps it and the `RenderState` the renderer is handed.

**Orientation is a quaternion and only a quaternion.** Euler angles are never
stored and never accumulated. A 6DOF ship flies straight up a shaft and keeps
going; there is no "up" to gimbal against.

Frames are mixed on purpose:

| Quantity | Frame | Why |
| --- | --- | --- |
| `position` | world | |
| `velocity` | world | momentum carries on where the ship was going, not where it now points |
| `thrusterSpin`, `aimSpin` | ship-local | x = pitch, y = yaw, z = roll |

Thrust is built in the local frame and rotated once per step into world space.

### Two angular channels

`angularVelocity()` is the sum of two channels that exist because they have
genuinely different limits:

- **`thrusterSpin`** — what the manoeuvring thrusters produce, from the
  keyboard axes. Capped by `maxAngularSpeed`.
- **`aimSpin`** — what the mouse asks for. Not capped.

This split is not decoration; it was arrived at by measurement. With one
clamped channel a full mouse swipe turned the ship **29 degrees instead of
180**: the impulse itself survived the step it arrived on, and the clamp then
caught what was left of it on the *next* step and ate the rest of the turn.
Raising the cap until it stopped biting would have made it meaningless for the
thrusters, which is what it is actually for.

Mouse motion is applied as an angular-velocity *impulse* rather than as a
rotation, so a turn carries the ship's rotational inertia and settles instead
of snapping. Because damping is what consumes the impulse, the total rotation
is `impulse / angularDamping` however the motion was spread across steps — a
swipe turns the ship by the same amount whether it arrived in one frame or
thirty. That invariance is the property worth preserving here.

### Damping is a rate, not a per-step factor

Both dampings are exponential rates in 1/s: `v *= exp(-damping * dt)`. A
per-step multiplier would silently change the feel if the step rate ever did.
The time constant is `1/damping` and a decay is visually finished at about
three of them, which is where the shipped defaults come from:
`linearDamping = 3.0` gives tau = 0.33 s, so full throttle reaches top speed in
about a second and releasing it coasts to a stop in about a second. Terminal
speed under exponential drag is `thrust / damping`, so `forwardThrust` is set
to `maxSpeed * linearDamping` and the speed clamp only catches the diagonal
case of several axes thrusting at once.

Every constant lives in `assets/config/flight.json`, is editable live in the
ImGui tuning panel, and can be saved back over the file.

### Input

`src/game/bindings.h` is the single table of GLFW key codes. It samples into
`sim::ShipControls`, which carries normalised axes and nothing device-specific,
so `src/sim` never sees a key code. Rebinding later means editing that array.

### `--flight-test`

`src/sim/flight_test.cpp` runs the real `stepShip` at the real fixed rate with
no window and no GL, and measures the three numbers M1 is tuned to hit:
spin-up to 95% of top speed, coast-down to a visual stop, and the rotation one
full mouse swipe produces. It exits non-zero if any is out of range. This is
what caught the clamp bug above — it was invisible by eye and unmistakable as a
number.

---

## Stencil shadow volumes

`src/render/shadow.h`. Silhouette edges extruded to infinity and counted in the
stencil buffer with depth-fail — the Doom 3 and F.E.A.R. technique. Pixel-exact
hard edges, no acne, no shadow-map resolution to run out of.

All of it is CPU work. The meshes are tens of triangles, so a geometry shader
would cost more in setup than it saves, and the CPU path is the one whose
correctness can be printed.

### Closure is not negotiable

`buildOccluder` welds by position, builds edge adjacency, and **fails fatally if
any edge does not have exactly two triangles**, naming the offending edge's two
positions. An open edge means the extruded volume has a hole, the depth-fail
counts come out wrong beyond that hole, and the shadow flickers or inverts
depending on where the camera is. There is no tolerable degraded behaviour, so
there is no warning path.

The mine itself is an occluder, and the most important one — without it a light
spills through walls into rooms it should never reach. **The union of every
segment's wall triangles is a closed surface**, because segments share vertex
indices across portals, so the walls of two joined segments weld along the
portal rim instead of leaving a crack. `--world-test` checks the Euler
characteristic comes out at 2 (one closed surface, genus 0). For `test01`:
V 58 − E 168 + F 112 = 2.

### Building a volume

For each silhouette edge — one whose two triangles disagree about facing the
light — a quad extruded away from the light, plus a front cap of the
light-facing triangles and a back cap of the same triangles reversed at
infinity. The volume must be closed for depth-fail to count correctly.

The extruded vertices carry `w = 0` and hold a *direction* away from the light
rather than a position. Under the infinite far plane (see "Reversed-Z") they
land exactly on the far plane instead of being clipped. **This is the reason
that projection exists.**

Side quads take the edge as the *light-facing* triangle winds it, which is what
makes the quad's outward normal point out of the volume — and therefore what
makes "front face" and "back face" mean what the stencil ops assume.

### Volumes are built in the occluder's own space

`appendShadowVolume` decides which triangles face the light in world space,
but emits the vertices in the **mesh's own space**, and each occluder's run of
triangles is drawn with the same model matrix the mesh was drawn with in the
depth prepass (`ShadowVolumes::Draw`). The direction to infinity is
`p − light` with the light brought into object space, since
`M·(p − l, 0) = M·(p, 1) − M·(l, 1)`.

This is the same invariance argument as the `GL_GREATER` section below, applied
to every mesh rather than only the mine. The front cap is the occluder's own
light-facing triangles and has to land at *exactly* the depth the prepass
wrote. Transforming to world space on the CPU and drawing with an identity
matrix computes the same point by a different association, and the last bit
differs: a cap an ulp in front of its own surface is not counted, and the
surface shadows itself in a fizz of scattered pixels. The mine's matrix is the
identity, so it never had the problem, which is why M3 looked right on the
walls while the ship, robots, pillars and door slabs all fizzed. Under
software GL a pillar under the headlight rendered almost entirely black before
this and fully lit after, and the chase-camera ship's hull went from speckled
to clean.

Matching the matrix and the expression is **necessary but not sufficient**.
It makes the cap's *vertices* bit-identical, and the depth test compares the
*rasterized interior* — which the hardware derives from whichever three corners
the triangle was built from. So the occluder has to be built from the same
triangle list, in the same corner order, as the mesh that is drawn. Every
caster derives both from one `ModelData`: the ship and the props from their
OBJ, the mine from its own wall triangles, and now the door slabs from
`cubeModel()`.

The door slabs were the exception and the last place this artifact lived. They
were drawn from `makeCube()` and cast from a second cube typed out beside them
in `Renderer`. Both were ±0.5 cubes, so every vertex matched to the bit; they
agreed on the +Z and −Y faces, rotated the corner order on +Y, and split −Z,
+X and −X across the *opposite diagonal*. Two triangulations of one flat quad
describe the same plane and still disagree by an ulp across the middle of it,
so under `GL_GREATER` scattered interior pixels stopped counting the cap, kept
the uncancelled back cap at infinity, and read as shadowed. A door slab's two
large faces are its ±Z: the +Z face was bit-identical and clean, and the −Z
face — always the same side of every door, since `Door::corners` is wound from
the owning segment's side — fizzed wherever a light sat close enough to make
the difference visible. The blob tracked the light's own falloff, because an
unlit pixel that goes further unlit shows nothing.

### `GL_GREATER`, and why it is not `GL_GEQUAL`

The volume pass uses `glDepthFunc(GL_GREATER)` where the rest of the frame uses
`GL_GEQUAL`. **This one comparison is the difference between working shadows and
a black screen.**

Depth-fail counts volume faces lying *behind* the visible surface. A volume's
front cap is the occluder's own light-facing triangles, exactly coincident with
the surface being lit, and it has to be counted so that it cancels the back cap
at infinity. Otherwise every lit surface finds one uncancelled face behind it
and shadows itself. In a conventional `GL_LESS` depth buffer a coincident
fragment *fails* the test and that cancellation is automatic. Reversed-Z with
`GL_GEQUAL` makes equal **pass** instead, the cap silently stops counting, and
the entire mine renders black. `GL_GREATER` restores the strict comparison
depth-fail assumes.

Getting there also required `invariant gl_Position` in `shadow.vert`, spelled
identically to `depth.vert` and fed the same `uModel` the prepass used for
that occluder. With a different association the coincident cap lands a bit
off, scattered pixels count as behind, and the walls crawl with stripes of
false shadow.

### Why depth-fail and not depth-pass

Depth-pass is cheaper and breaks the moment the camera is inside a shadow
volume — which, in a mine lit by a headlight, is most of the time. Counting what
is behind the visible surface instead of what is in front of it makes the near
plane irrelevant.

### The segment-set boundary rule

The mine's shadow volume is built from the **entire closed wall surface**.
The light's segment set still limits receivers and selects separate object
casters, but must not cut triangles out of the mine occluder.

The former subset rule omitted boundary edges and left the emitted volume
open. Light falloff does not make that safe: depth-fail counts intersections
behind a visible receiver, including geometry beyond the light radius. Missing
boundaries can therefore suppress light on nearby walls as black polygons.

`appendShadowVolume` is shared by rendering and the headless world test. The
test checks oriented edge cancellation on the emitted homogeneous triangles,
including finite caps and caps at infinity, for every static level light.
This validates the generated volume rather than only the source mesh. Building
the complete mine increases volume work; performance tuning must preserve closure.

### Anti-aliasing

`GL_SAMPLE_SHADING` with `glMinSampleShading(1.0)`. Where the stencil differs
between samples inside one pixel — which is exactly a shadow edge — the light
pass shades per sample. Without it the multisampled stencil buys nothing and
the hard edges come out stair-stepped, which is the one artifact this technique
is supposed to be free of.

### The headlight

A shadow-casting spotlight on the ship, toggled with **H**. Its segment set is
rebuilt every frame through `LightFlood`, whose scratch buffers are reused so
the draw path does not allocate.

**It starts off.** The mine opening in darkness, with H the first thing the
player reaches for, is the introduction a game with no ambient term deserves;
starting lit gives that away before the level has said anything. Because a
frame-limited run therefore has no way to show the headlight without a
keypress, `--headlight` turns it on for a screenshot — the same gap `--chase`
and `--automap` fill. The flag sets it on the session's **initial** state, not
the live one: `start()` and every restart copy that over the current state, so
anything set only on `current()` lasts until the run begins and no longer.

**Wide and soft rather than narrow and hot**: a 60° cone at intensity 2.0,
over a radius of 62. The earlier 34° at 3.2 put a hot spot on one wall with
nothing either side of it, which read as a torch rather than as a ship's lamp
and left the player sweeping the beam to find a corridor. Trading on-axis
intensity for solid angle — ×0.625 and ×3.07 respectively — lights the shape
of the corridor instead of a circle on its wall, and leaves the level's own
lamps as the brightest things in the mine, which is what makes flying towards
one feel like progress.

**The ship is never an occluder for its own headlight.** A lamp bolted to the
hull would spend every frame shadowing the hull it is bolted to, and the view
would be a black cone. The ship is also not an occluder in first person at all:
the camera sits inside the hull, so its volume would begin at the eye and black
out most of the view — the player shadowing themselves with a mesh they cannot
see. It is not drawn in first person either, so what casts a shadow and what is
visible stay in agreement.

---

## Collision and the query API

`src/sim/collision.h`. A `Body` is a position, a velocity, a radius and a
segment. The ship is one; robots, projectiles and debris become Bodies later.
**Collision is sphere-based and only sphere-based** — a 6DOF shooter's ships and
robots are compact and the mine is large flat faces, so an oriented box costs
more in cases to get wrong than it buys in fit.

### The resolve loop

Each step, after integration: push the sphere out of any wall it is inside,
remove the inward component of its velocity so it slides, repeat up to four
times. Corners need more than one pass, because resolving against one wall can
push the body into another.

Two details carry the whole thing:

- **The push is away from the closest point on the triangle, not along the
  triangle's normal.** On a face they agree; on an edge or a corner they do not,
  and using the normal there slides the body along the wall instead of off it.
  That is exactly the case where a sphere resting on a triangle's edge falls
  through. `closestPointOnTriangle` in `src/sim/geometry.h` handles the face,
  the three edges and the three vertices separately for the same reason.
- **The candidate set is the body's segment plus every segment through one of
  its portals.** A body that crossed a portal mid-step is up against the
  neighbour's walls, not its own, and a body wider than a portal has to be
  stopped by the rim rather than squeezed through it.

Only the inward part of the velocity is removed, so the tangential part
survives and the ship slides. While in contact, `wallScrape` damping bleeds
speed, so grinding along a wall costs something.

### Queries

`raycast` walks the segment graph: test the current segment's walls, and on
leaving through a portal continue into the neighbour. `sweepSphere` does the
same for a sphere and is what M5's projectiles move along, so a bolt crossing
several segments in one step cannot pass through a wall.

The sweep uses **conservative advancement** — step forward by the distance to
the nearest wall, which can never step past it — rather than the analytic
swept-sphere test. The analytic form is faster in the worst case but has
separate face, edge and vertex branches, and those branches are where a
projectile system finds its bugs. This form is a handful of lines, cannot tunnel
by construction, and is deterministic. Its weakness is grazing contact, where it
converges slowly and gives up, reporting no hit — which for a projectile means
it keeps flying, the safe answer.

Neither query allocates.

### Bodies against each other

`resolveBodies` in `src/sim/collision.h`. The ship, every live robot and every
floating light go into one `BodySet` each step, and the pass separates them from
each other, from the level's objects, and from the mine.

**It cannot be done a body at a time.** A pair needs both masses at once, and —
less obviously — the body pass has to be **interleaved** with the wall pass
rather than sequenced before it. Push two ships apart and then push each out of
the walls, and the wall correction can put them back inside each other. Do it
the other way round and one of them goes through a wall. So the loop alternates,
bounded, and **always ends on the mine**: a body outside the world is worse than
two bodies overlapping, because it tunnels projectiles, walks robots through
rock and stops doors blocking.

Order within an iteration is fixed — pairs by slot index, then objects, then
walls — because sequential resolution is order-dependent and the fixed timestep
is meant to be reproducible. `--collision-test` runs the same four-body setup
twice and requires bit-identical results.

The wall pass inside `resolveBodies` is a **correction**, not the primary one.
Each system still resolves its own bodies against the mine on the way in, with
its own scrape damping and restitution; this pass re-resolves only the bodies
that a contact actually moved, with damping zeroed so that grinding along a wall
does not cost double for any body that happened to touch another one. When
nothing touches anything — the common case — the whole pass is one sweep of
distance tests and it stops.

**No broadphase.** `kMaxRobots` is 32 and every body carries a segment, so the
pair loop is gated on the segment graph and brute force over the survivors beats
any acceleration structure.

#### The segment gate, and why world distance is not enough

Segments share their vertices, so two rooms side by side are separated by a wall
of **no thickness whatever**. Two bodies hugging that wall from opposite sides
are well inside each other's radii in world space while having no business
touching; without the gate they shove each other through solid rock and the wall
pass spends the rest of the step undoing it. A pair resolves only if they are in
the same segment, or one portal apart with that portal not shut.

Objects are narrower still — same segment only. An object sits where a level
author put it, which is in the middle of a room rather than against a portal,
and widening it to neighbours would let a pillar standing against a wall push a
body in the room beyond. The cost is that a body straddling a portal does not
collide with a pillar planted right beside it; no shipped level does that.

#### Which entries are solid

`SolidObjects` carries the `alive` flags from `ObjectHealth` as a raw pointer,
so collision does not have to know what combat is. It does two jobs: a robot's
spawn entry is **not** solid — the robot list owns it and the robot's own body
is already in the set — and a destroyed object stops colliding, so you can fly
through where a crate stood. Without the first, every robot spawn point becomes
an invisible box that ships bounce off, which is the same bug `initObjects`
already fixed once for gunfire.

#### The impulse goes into `knockback`

Not `velocity`. That channel is owned by the flight model, which damps toward
zero and clamps to `maxSpeed`, so a shove from a brute would be gone in the step
it arrived in. `knockback` exists for exactly this — velocity the body did not
ask for — and momentum is conserved on `motion()` regardless of which channel
receives it, because each body's share is one impulse scaled by its own inverse
mass with opposite signs.

`Body::mass` is new. The three authored values — `RobotDef::mass`,
`FloatingLight::mass` and `WeaponConfig::playerMass` — stay where they are; the
body is where the resolve reads them from, because a pair needs both at once.

#### What is conserved, and what is not

The first draft of this proposed asserting that **total speed never grows**.
That is true against a wall, which is effectively infinite mass, and false
between two bodies of different mass. 100 into 1 at rest, elastically, takes the
sum of speeds from 1 to 2.96 while kinetic energy does not move — so the
assertion would have failed a textbook-correct collision.

What `--collision-test` checks instead:

- **Momentum conserved**, for an isolated pair only. A wall is infinite mass and
  positional push-apart injects position without touching velocity, so either
  one present invalidates the check. Measured drift: 7.6e-08.
- **Kinetic energy not increasing**, for restitution ≤ 1.
- The speed sum is measured and **reported** alongside them, at 1.000 → 2.960,
  so the reason the invariant changed is in the log rather than in a commit
  message.

#### Tunnelling, asserted rather than closed

Body-against-body is a discrete overlap test, so in principle two bodies can
cross between steps. The smallest pair in the game is the ship at 1.6 and a
drone at 1.9. At 120 Hz, even at twice the maximum head-on closing speed — 80
u/s, leaving headroom for knockback — they cover 0.667 units in a step against
3.5 units of combined radius. The test asserts that margin instead of paying for
continuous collision forever to cover a case the tuning cannot reach; it fails
loudly the day someone raises `maxSpeed`.

#### The box is no longer a box

A body resting against `mine01`'s pillars used to settle up to **0.92 units**
past the column's own widest radius, because the proxy was the model's bounding
box and the column is a tapered octagon — 79% more volume in the collider than
in the thing being collided with. Columns now get a tapered-cylinder proxy and
the same measurement reads **0.09 units**, against the column's radius at the
height the body actually came to rest at. See *Proxy volumes* below.

### `--collision-test`, and four wrong oracles

The test flies the ship into every wall of the level from all 26 face, edge and
corner directions at max speed, re-applying full throttle every step so the body
is pressed into the wall continuously rather than bouncing off once. It also
slides along every wall at 45°, walks every portal from both sides, and
cross-checks both queries.

The interesting part was the oracle — "is this point inside the mine?" — which
was wrong twice before it was right:

1. **`findSegment`.** It walks the segment graph testing one averaged plane per
   side. That is the cheap traditional test and it is approximate for a side
   that is not planar, which every side in this format may be. It reported 125
   of 364 raycasts as disagreeing.
2. **Nearest wall triangle, which side of it.** Wrong at a *convex* edge, where
   the mine's surface folds towards the interior — as it does at the inside of
   every bend. Beyond such an edge the nearest point is *on* the edge, and the
   direction back to the query point has a negative dot with both adjacent
   faces' normals, so an interior point reads as outside. Nine false escapes.
3. **Ray parity.** The wall union is one closed surface (M3 validates that), so
   a ray from an interior point crosses it an odd number of times. Three
   deliberately off-axis directions and a majority vote, because a single ray
   that grazes an edge double-counts it. Zero false anything.

The raycast half of the test had a second oracle — "how far to the wall?" — and
that one was wrong twice more. Both attempts marched a point along the ray in
0.05-unit steps until it left the mine:

4. **Marching on the parity test alone.** A ray along a grid diagonal can pass
   through the single point where two segments meet at a corner. Parity still
   says "inside" there, so the march walked out through the corner and carried on
   to a wall much further away than the one `raycast` correctly stopped at.
5. **Marching, plus a clearance requirement.** M7 closed that hole by also
   requiring the point to stay a step clear of the surface, which opened the
   opposite one: a ray that *legitimately* grazes a corner now ends the march. On
   `mine01` a ray from segment 16's centre along (−0.71, 0, −0.71) clears a
   portal corner by 0.0202 units and continues into segment 14 — the march called
   that a wall at 13.850 units and reported `raycast`'s correct 71.023 as a
   failure. It stood as M7's one outstanding failure until it was traced.
6. **Every wall triangle, nearest hit.** The wall union is closed, so the first
   triangle the ray meets *is* the wall it hits. No stepping, no threshold, and
   no segment graph — that last part is what keeps it independent of `raycast`,
   whose whole job is to walk that graph. 244 triangle tests per ray on
   `mine01`. The two now agree to 0.000011 units on both levels, so the
   tolerance is 0.001 rather than 0.35; a deliberately injected 0.01-unit error
   in `raycast` is caught on all 884 rays.

The collision code was correct throughout — all four times. **If a test
disagrees with the code, the test is a suspect too**, and on this one it was the
culprit every time. The last of the four is worth the reminder: it was recorded
in `MILESTONES.md` as a collision bug, with a diagnosis that changed between
runs, and it was never collision at all.

---

## Weapons, projectiles and the light budget

`src/sim/combat.h`. Projectiles, flares and effects are **fixed-capacity arrays
held by value in `SimState`**, because `SimState` is copied every step and a
`std::vector` there would allocate in the simulation loop. A full pool drops the
newest request rather than growing.

### Every bolt is a light

A laser going down a corridor sweeps shadows off everything in it. That is the
most cinematic thing the technique buys, and it costs one flood fill per bolt
over a radius small enough that the fill stops almost immediately.

Projectiles move by **swept sphere**, not by integrating position and checking
afterwards. `--weapons-test` fires a bolt at 2000 units/s — 16.7 units per
120 Hz step, most of a segment — and it still stops at the wall.

### The shadow budget

At most `shadowBudget` lights (default 16) **cast** per frame. Every light
still *lights*; the budget only decides which also cast, because casting is
what costs: a silhouette extraction and a stencil pass per light per frame.
Under sustained vulcan fire there can be thirty bolts in the air, and thirty
stencil passes is not a frame.

A light that lights without casting shines straight through every robot,
pillar and wall inside its segment set, so **whichever lights lose the budget
leak**. The order decides who leaks:

1. The headlight. It is the light the player looks along, and the one whose
   shadows move with every turn. It used to be claimed after the level lights,
   so in any room with eight visible level lights it got no slot and lit
   through everything — which read as light leaks, and was why a budget of 32
   always looked better than 8.
2. The level's own lights the portal walk can see, **nearest to the camera
   first**. An off-screen light casting into nothing is the cheapest thing to
   give up, and a distant one is the next cheapest.
3. Everything dynamic, nearest first. A bolt across the room contributes a
   shadow nobody can resolve.

The default moved from 8 to 16 with the reorder. Since the whole closed mine is
extruded for every casting light, the right number is a measurement for M8 on
the real GPU; the slider goes to 32.

The overlay reports slots used and how many lights lit without casting, so the
cap can be watched rather than assumed.

### Resources

The headlight drains energy and **switches itself off at zero**. In a game with
no ambient term that is a real consequence, which is the point of it costing
anything. Shields at zero resets to the player start.

### Objects, robots and targets

`Level::objects` is one list, but two systems own its entries. An object whose
type names a robot definition is spawned as a robot and stepped by
`stepRobots`; everything else is static scenery with health in `ObjectHealth`.
`initObjects` marks the robot entries **dead from the start**, so the object
hit test never sees them. Before that rule every robot's spawn point stayed
behind as an invisible, indestructible sphere that stopped shots for no visible
reason — sixteen of them in `mine01`. The weapons test now fires through a
removed robot's spawn point and fails if the bolt stops.

The renderer builds its prop list from the same objects, skipping robots, so a
prop's index and its object's index are different numbers; `Prop::objectIndex`
carries the mapping for the alive check.

Homing acquires **robots first**, then destructible scenery, and steers at
whichever it locked; a target that dies is flown past.

### Proxy volumes

`src/sim/proxy.h`. Every object carries a `PlacedProxy` — a sphere or an
oriented box, derived from its own model's bounds at load and parallel to
`Level::objects` in `Level::objectProxies`.

Before it, the combat trace tested every object as one hardcoded 2.4-unit
sphere on its origin. `pillar.obj` is a column 4.4 × 18 × 4.4, and `mine01`
places both of its pillars at `y = 9`, so each spans `y = 0…18` — **only the
middle 4.8 units of an 18-unit column stopped a shot.** Aim at the top of a
pillar and the bolt went through it and hit the wall behind, in full view of
the player. One radius cannot serve both a crate and a column.

Not the mesh, though. A pillar is 28 triangles and the renderer already
extrudes every one of them into a stencil volume, but nobody can resolve an
octagon's facets at the speed a ship crosses a room, and per-triangle queries
in the sim step are where a projectile system finds its bugs. The box is one
clamp where the triangle test needs seven barycentric regions, and it has no
edge or vertex case to get wrong.

#### Three kinds, because a box around a column is mostly air

A bounding box is the right shape for a crate and the wrong one for a column.
`pillar.obj` is a tapered octagon, 2.2 units of radius at the base and 1.7 at
the top, and the box drawn round it stands 3.11 units out at its corners and
never narrows. A body could come to rest **1.4 units clear of anything
visible**, and because `placeProxy` folds the entity's scale into the half
extents, scaling a pillar in the editor multiplied that gap by the scale: at 3x
it is 4.2 units, more than two ship radii of open air. That is the divergence
between collision and model that this kind exists to close.

So `ProxyVolume::Kind` has a third member, `TaperedCylinder`: a conical frustum
about the model's own Y axis, carrying a radius at each cap alongside the half
extents every kind already has. `halfExtents` keeps its meaning for all three —
the box that contains the solid — so `boundingRadius`, the broad-phase reject in
`occluded`, and anything that only wants to know how big a thing is are
unchanged. For a frustum that radius is the rim of the wider cap rather than a
box corner, which is exact.

The radius is the ring's **circum**radius, not its apothem. An octagon inscribed
in its own circumscribing circle leaves 7.6% of the radius between the two at
the flats — 0.17 units on `pillar.obj` — and that is the residual this kind
does not remove. It is deliberately in that direction: the collider contains the
mesh everywhere, so a shot always stops at something the player can see. An
apothem-fitted cylinder would be tighter on the flats and let bolts through the
column's own corners, which is the failure the proxy work existed to fix in the
first place.

Three primitives in `sim/geometry.h` do the work, beside the box's own:

- **`closestPointOnTaperedCylinder`** reduces to two dimensions — a frustum is a
  solid of revolution, so the problem is the nearest point on a trapezoid in the
  (radius, height) half plane, swung back around the axis. Its three faces are
  tested outright; a clamp would only be correct if the side were axis aligned,
  and on a taper it is not.
- **`rayTaperedCylinderDistance`** clips to the caps with the box's own slab
  test and solves one quadratic for the side. Which stretch of the ray that
  quadratic calls *inside* depends on its leading coefficient — between the
  roots normally, but **outside** them for a ray running up the taper within the
  cone's own half angle, and the whole span when there are no roots. Rather than
  a branch per case the span is cut at the roots and the sign is evaluated at
  each piece's midpoint, which answers all three identically. The frustum is
  convex and bounded, so exactly one stretch survives.
- **`taperedRadiusAt`** is the profile itself, and is what the tests measure
  against so that a taper is not mistaken for a gap.

`proxySphereContact` grows a matching penetration case. A body whose centre is
inside leaves through the nearest face, as it does for a box, but *nearest* is
measured along the slanted side's own normal rather than straight out from the
axis — on a taper those differ, and the perpendicular distance is the one that
says which face is really closest.

Inflating for a projectile's own thickness is the one place the frustum needs
arithmetic a box does not. Growing the caps by the bolt's radius is right;
growing the radii by it is not, because the side moves out along **its** normal,
which measured radially is `sqrt(4h² + Δr²) / 2h` times as far. On `pillar.obj`
that factor is 1.0004 and on a steep cone it is not, so it is computed rather
than assumed.

#### Which models get one, and why the test is strict

`gl::modelTaperedCylinder` decides, at load, from the model's own vertices: it
buckets them into rings by height, requires every ring to be regular (one radius
within 1%, at least six vertices, no angular gap over 61°) about a shared axis,
and requires the radius to run **straight** from one cap to the other within 2%.
Anything it is unsure of it rejects and the caller falls back to bounds.

Strict on purpose, because a false positive is a collider that does not contain
its own mesh — the exact bug the proxy work was for, reintroduced. Six sides is
the floor because a square's corners stand 41% proud of its flats, and
circumscribing one would be far worse than boxing it; this is also what keeps a
crate a box. Of the shipped models exactly two pass: `pillar.obj` (2 rings of 8,
92% round) and `drone.obj` (2 rings of 6, 87% round, and never read — a robot's
body carries its authored `def.radius`). `reactor.obj` and `brute.obj` are
rejected because their radius goes back up again partway; `gunner.obj` and
`ship.obj` are not solids of revolution at all.

The seam is deliberate, and it is the reason this took a new file rather than
an include:

- **`sim` still never reads an asset.** `ProxyVolume` is glm and nothing else.
- **`render/model_data.h`** is the GL-free half of `render/model.h`, split out
  so that something can have a model's geometry without its drawable form.
  `model.h` pulls in `render/mesh.h` and therefore `render/gl.h`.
- **`game/proxies.cpp`** is the only place that sees both. It reads OBJ bounds
  and writes finished `PlacedProxy` values into the level.

`resolveObjectProxies` is called from **`loadLevel`**, not from each caller.
Eleven headless tests, the asset validator and the game all load levels, and a
proxy table one of them can forget to build is a table that is empty in exactly
the test meant to catch it being empty.

Robot entries get a proxy too, and it is never read: `initObjects` marks them
dead so the robot list owns them, and a robot's body carries its authored
`def.radius` rather than its model's bounds. Resolving them anyway keeps the
function independent of the robot config, which `loadLevel` does not have.

A projectile with a thickness of its own inflates the volume by its radius.
For a sphere that is exact; for a box the true swept shape has rounded corners,
so a shot passing within the bolt's radius of a corner counts as a hit where
strictly it grazed past. That errs towards stopping at something the player can
plainly see, which is the direction to be wrong in.

`--weapons-test` fires at the bottom, middle and top of the level's
column-shaped object and requires all three to hit. It also reports how far the
old 2.4-unit sphere sat from each ray and **asserts that the two ends really
were outside it** — 5.6 units off in `mine01` — so the test is provably about
the bug rather than three shots that happened to work.

### Fire points

`kShipFirePoints` in `src/sim/ship.h` must stay on the ship model's widest
vertices — `assets/models/ship.obj` puts them at x = ±2.0 — or bolts appear out
of thin air beside the hull. Laser and vulcan alternate between them.

### Effects

Impact flashes and explosions are short-lived additive billboards in the unlit
pass, expanded on the CPU into one buffer and drawn in one call. There is no
particle system beyond that, deliberately.

---

## Doors

A door is a **portal in the graph** carrying a slab that fills the opening
while it is shut, rather than a new kind of side. The two segments still share
their four vertices, so the mine's wall surface stays closed and the shadow
occluder still validates.

Two views of the same state exist, both derived from `GameState::doors` every
step:

- **`DoorField`** — two bytes per segment side. `blocked` is what a body
  cannot get through, set while openness is below `kPassableAt` (0.75).
  `opaque` is what nothing can be seen or lit through, set only while the
  door is **fully shut**. The portal walk and the light flood consult
  `opaque`, so the room behind a door comes into view the moment it cracks
  open; until then the walk stops and the room is not drawn at all.
- **`SolidDoors`** — the world-space triangles of every shut door, built by
  `buildSolidDoors` with the `blocked` threshold and handed to
  `resolveCollision`, `raycast` and `sweepSphere`. Answers "where is the
  surface a body or bolt stops at".

The level's own lights get their segment sets at load with no door state.
`main.cpp` rebuilds them all against the `DoorField` at start and again
whenever the number of opaque sides changes, and the headlight's per-frame
rebuild takes the field too. Before that, three of `mine01`'s fourteen lights
reached both sides of door 0 and lit the far room through the doorway, held
back only by the slab's shadow volume — which is where the fizzing artifacts
on one side of a doorway came from.

The door slab halves are oversized by a quarter unit at the frame and at the
top and bottom, so their outer faces sit inside the rock rather than exactly
in the plane of the adjacent wall; coplanar faces from two meshes z-fight in
the prepass and then disagree in every light pass. The halves are always drawn
and always cast: fully open they are buried in the wall, and while opening
they are the only thing occluding the doorway.

`main.cpp` builds `SolidDoors` once per step and passes the same pointer to the
ship's collision, `stepCombat` and `stepRobots`. **Nothing geometric runs
door-blind.** The first M7 build handed it to the ship only, so robots saw,
shot and flew through shut doors and bolts from either side passed through.
The loop test now fires at a shut door, parks a robot behind one, and fails if
anything gets through.

Robots carry no keys, but an unkeyed door opens for anything that comes near.
So the path search treats a **locked** shut door as a wall and an unlocked one
as a corridor: a robot walks up to an unlocked door and it opens, while a keyed
door stays locked until the player opens it. The distance field is rebuilt
whenever the number of shut door faces changes, not only when the player
changes segment.

---

## The game loop (M7)

`src/sim/gameflow.h`. Find the reactor, destroy it, get out before the mine
does. One `stepGame` runs doors, pickups and the win/lose flow inside the fixed
step, in that order, and it is the only place `GamePhase` changes.

`GameState` holds the whole of it: the phase, the countdown, elapsed time, the
shake, the robot tally, which pickups are gone, which keys are held, and the
`DoorField`. It lives inside `SimState`, which is **copied every step**, so it is
fixed-capacity arrays and scalars throughout — trivially copyable, with nothing
that owns memory. The one pointer in it is `endReason`, and that points into
string literals rather than at anything it has to free.

### Four phases, and one direction through them

`Playing → Countdown → Won`, or either of the first two → `Lost`. `stepGame`
returns immediately in `Won` and `Lost`. The phase never runs backwards;
restarting rebuilds the state from the level instead of unwinding it.

### The application owns the run

`src/game/session.h` separates application state from level state. `AppMode`
tracks the main menu, active play, pause and the run summary while `GamePhase`
continues to describe only events inside the simulation. That keeps a pause
menu from becoming a simulated phase and gives later player-facing screens one
place to decide whether the fixed step may advance.

`LevelSession` owns the initial, previous and current `SimState`. Starting a
level builds all three from the level and robot definitions. Restart copies the
stored initial state back into both interpolation endpoints, so no velocity,
projectile, door position or render interpolation survives. Leaving a run
returns the application to the main menu without trying to reverse simulation
events.

Only `AppMode::Playing` admits fixed steps. Entering pause clears the
accumulator, and resuming begins with an empty accumulator, so time spent in a
menu cannot turn into a burst of catch-up simulation. Paused and summary time
is excluded from the achieved-step-rate denominator for the same reason that
minimized time is excluded.

The reactor is not a special object type. It is a **robot whose definition sets
`isReactor`**, which means it already has health, line of sight, a weapon and a
shadow-casting mesh, and the flow only has to ask whether any live robot has
that flag. Destroying it starts the countdown, unlocks the exit, and leaves
every other robot fighting.

The three endings are `escaped`, `the mine went up with you still in it`, and
`shields gone`.

### The countdown is felt before it is read

`shake` ramps as `fraction * fraction` over the countdown, fraction being how
much of it has gone. `main.cpp` turns that into two things: a camera offset whose
three axes each sum sines at unrelated frequencies, so it never settles into a
visible rhythm, and `staticLightScale`, a flicker that only ever **dips** —
the lights are failing, not sparkling.

Both read `clock.seconds()` directly rather than integrating per frame, and both
land on `RenderState` rather than on the ship. So the shake looks the same at 30
fps as at 300, cannot bank motion across a stall, and never moves the ship's
actual position — it is a view of the mine coming apart, not a force on the
player. A player who never looks at the timer still knows how long is left.

### Keys open one door each

A door names a key colour; `hasKey` gates it. A locked door does not
acknowledge the player at all without the key — it does not begin to open and
then stop, because a door that twitches reads as broken rather than locked. The
loop test holds **every wrong key** against every door, not just the right one,
which is what catches a mask or index slip that a single positive test would
pass.

Pickups are a touch test against `player.radius + 2.4` with a `collected` flag,
so a pickup cannot be taken twice and the level file stays immutable. Keys
persist until the level ends; everything else applies its effect to
`CombatState::player` on contact.

---

## The HUD

`src/render/hud.h`. Screen-space quads and text, built by the game layer into a
`HudData` struct and drawn after the MSAA resolve, on the default framebuffer,
alongside ImGui. It is axis-aligned and gains nothing from multisampling, so
resolving first costs nothing and keeps the HUD out of the shadow and lighting
passes entirely.

`HudData` is a **flat struct of values the game layer fills in** — bars, weapon
name, ammo, keys, countdown, phase, message. The renderer never reaches into
`GameState` or `CombatState` to draw the HUD, which is what keeps `src/render`
free of simulation types.

### The reticle follows the guns, not the window

The reticle is the one part of the HUD that is not screen furniture: it marks a
place in the world. So it is **drawn over the aim point, projected with the same
matrix the scene was drawn with**, rather than nailed to the centre of the
window.

In first person the distinction does not show. The camera sits on the firing
line, every point on that line projects to the same pixel, and that pixel is the
centre. The chase camera is a different thing entirely: it looks at the ship
from `kChaseOffset` - above and behind - at a point `kChaseLookAhead` ahead of
the nose. Only that one point is at the centre of the window. The rest of the
firing line climbs away from it: at the default 70 degree vertical FOV and 720p
it is 61 px above the centre 40 units out and 89 px at the vanishing point, and
a screenshot of the opening corridor puts the reticle 78 px up. A centred
reticle in chase view marks a spot on the floor some way short of anything a
shot would hit.

The aim point comes from the raycast `main.cpp` already fires straight out of
the nose each frame for the debug overlay: the first wall the shot would meet,
or a point far along the line when it meets none. Two details that are not
decoration:

- **Clamped to 12..200 units.** Under the near bound the reticle swings across
  the screen for a couple of units of travel when the nose is against a wall;
  over the far bound it has stopped moving for any practical purpose.
- **Chased, not set.** Crossing a corridor mouth steps the hit from a near wall
  to a far one in a single frame, which in chase view is a reticle jumping tens
  of pixels. The tracked distance lags it at 12/s. The lag lives in `main.cpp`,
  where the frame delta already is, so the renderer stays a function of what it
  is handed.

`HudData::hasAimPoint` is false on the automap, which has its own camera and no
firing line on screen; the reticle falls back to the centre of the window there,
as does anything the projection cannot place (a point at or behind the eye).

### The darkness gauge

The mine has no ambient term, and `playerVisibility` (see *Darkness hides you*)
turns that into a rule robots act on: an unlit player they have never seen
cannot be picked out. That rule was invisible. A player flying a black corridor
had no way to know whether it was dark enough to count, and the difference
between hidden and seen is a step or two of flying.

So darkness is a third gauge on the energy/shields stack rather than an icon of
its own. It is a live quantity the player spends and recovers by flying,
which is exactly what the other two are:

| Part | Shows |
| --- | --- |
| The bar | The light reaching the player |
| The tick inside it | `sim::kDarkThreshold`, the value it must stay under |
| The word | What the robots conclude — `hidden`, `exposed`, or `headlight` |

The tick is drawn **in** the bar, not as a colour change at the boundary,
because "how close am I to being seen" is the question worth answering while
flying; which side of the line you are on you can already tell from the word.

Two decisions worth keeping:

- **The word and the bar read one query.** Both come from
  `PlayerVisibility::illumination`, not from the hull-sampled `Illumination`
  main already computes for the debug panel. The hull query is the
  better-behaved number — it averages seven samples, so it does not flicker as
  a shadow edge crosses a point — but it answers a *different* question, and the
  two genuinely disagree near a shadow edge. A bar sitting below its own
  threshold tick next to the word `exposed` reads as a bug. The number the
  robots act on is the one that gets drawn.
- **The headlight names itself.** `playerVisibility` makes a player carrying a
  lamp visible whatever is falling on them, so with the headlight on in a black
  corridor the bar sits near zero and the answer is still "seen". Reading
  `exposed` over an empty bar looks broken; reading `headlight` says what
  happened and points at the key that undoes it. That state is exactly
  `headlight && illumination < kDarkThreshold` — the case where `visible` would
  have been false without it.

The headlight cannot brighten its own bar, which is what makes that state
readable at all. It sits `kHeadlightForward` (2.2) ahead of the ship's centre
pointing forward, and the query's sample points are at most `shipRadius` (1.6)
from that centre, so every sample is behind the cone's origin for any
orientation.

The word doubles as the gauge's caption, where `energy` and `shields` put a
fixed noun. A noun and the state together do not fit across the bar at this
scale, and between the two the answer is worth more than the name of the thing
being measured.

### The font is generated, not shipped

Text needs digits, a colon and capitals: 37 glyphs. Those are a 5x7 bitmap per
glyph in a table in `hud.cpp`, blitted into one RGBA8 atlas at startup with a
pixel of padding per cell so linear filtering cannot bleed neighbours together.
No font library, no shipped atlas, and the dependency allowlist stays where
CLAUDE.md puts it.

### Two state changes that produce nothing, silently

Both were found by an empty HUD and no error anywhere, and both are commented at
the call site because neither fails loudly:

- **Culling must be off.** The shader flips y to put the HUD's origin at the
  top-left, which is how a HUD is naturally laid out — and which reverses the
  winding of every quad. With culling on, the entire HUD is culled.
- **Depth test must be off**, since the HUD is drawn after the resolve with no
  depth buffer attached that would pass.

ImGui is debug-only from this milestone and starts hidden; F3 toggles it,
`--debug-ui` starts with it open, and `--no-ui` keeps it shut so a screenshot
shows the game.

### Player-facing menus

`src/game/menu.h` owns menu state, navigation and layout without depending on
OpenGL. It produces a `MenuView` containing fixed-capacity button data and
framebuffer-space rectangles. Keyboard and pointer input both resolve to the
same `MenuAction`, so clicking a button and confirming a focused button cannot
drift into separate behavior.

The renderer consumes that view through the existing HUD batch. Menus are
therefore screen-space text and rectangles drawn after the MSAA resolve, with
no new shader, no new pass and no new dependency — one texture, for the logo
below. The same rectangles drive pointer hit testing and rendering. Cursor coordinates are scaled from GLFW window space to
framebuffer space so high-DPI windows do not move the clickable area away from
the visible button.

#### The logo on the main menu

The main menu is topped with the game's wordmark instead of its title text; the
other screens keep their text, because "paused" is a label rather than a brand.
The art is `assets/images/logo.png`, a copy of the Steam library logo in
`marketing/steam/`. It is a copy on purpose: `assets/` is what ships beside the
executable, `marketing/` is sized and re-exported to a storefront's
requirements, and the two should be free to move independently.

Two things happen to it at load, in `render/sprites.h` beside the pickup art:

* **It is trimmed to its opaque bounds.** A store logo is padded out to a fixed
  canvas — the one shipped here is a 1029x193 wordmark inside 1280x300 — and
  centring that canvas would put the wordmark wherever the exporter left room.
  Trimming means the menu centres the picture and takes its aspect from the art,
  so a re-export at another size needs no change in the renderer.
* **The colour of its edge pixels is carried out into the transparent ones.**
  This is the fringe `loadPickupSprites` avoids by premultiplying, arriving by
  another road: the export is transparent *black* outside the letters, and every
  filtered sample near an edge — bilinear minification and each level of the mip
  chain — averages that black in and rings the wordmark in grey. Premultiplying
  is not the fix here, because the HUD blends straight alpha rather than
  additively, so instead the colour under the transparent pixels stops being
  black. Nothing reads it where alpha is zero.

It is its own single-layer texture rather than another layer of the comm array,
because a texture array's layers all share one size and a logo is not the shape
of a comm frame. `hud.frag` gains a second optional sampler for it, selected by
a UV band the way the image array already is, so the menu still draws in one
call with the rest of the HUD. Both samplers are bound once at startup as well
as per pass, because that fragment shader is shared with the world-space door
signs and must never see an incomplete texture on a unit.

A normal launch prepares the level as a frozen background and opens the main
menu. Command-line runs still begin in play so screenshots, resize runs and the
existing harness retain their behavior; `--start-menu` selects the menu for a
visual check. Pause and run summary use the same menu model, while ImGui remains
debug-only.

### Choosing a level

`play` opens a list of the levels in `assets/levels` rather than starting one.
`src/game/level_list.h` reads only the `name` and `comment` fields out of each
file: the list is a line of text per level, and loading every mine to draw it
would pay for a level's worth of geometry per entry. The same directory walk
serves `--validate-assets`, which was doing it privately for the same reason.

The list is the one menu whose length is not known at compile time, so the
cursor and the rows are deliberately different things. The cursor walks all of
the levels plus `back`; the view shows a window of at most eight rows centred
on the cursor. The window is computed from the selection alone, which keeps
`view()` const and pure and means there is no scroll position to fall out of
step with what is selected. Rows shrink before the window does and the window
shrinks before anything leaves the panel, so a 640x360 window shows fewer
levels rather than drawing them off the bottom.

A level's `comment` is written for whoever opens the file - mine01's is a
paragraph about where the keys are - so the menu keeps its first sentence and
the renderer elides what still does not fit at a word boundary. That path runs
every frame a menu is up, so it works in `string_view` and never builds a
`std::string`.

### Persistent player settings

`src/game/settings.h` owns the player-facing configuration that is independent
of authored flight tuning: master volume, mute, mouse sensitivity, mouse-Y
inversion, first- and third-person vertical field of view, fullscreen, window
size and vsync. On Windows it lives under
`%LOCALAPPDATA%/descent-gl/settings.json`; other desktop builds use the XDG
configuration directory when one is available. `--settings PATH` redirects it
for automated and portable runs.

The file is loaded before the window is created because fullscreen, size and
vsync affect construction. Missing files use defaults. A malformed file emits
a warning and uses defaults, while a partial or newer file keeps every field
that can be read and ignores fields this build does not know. Numeric values
are normalized before use. Saving writes a complete versioned document to a
temporary sibling and replaces the destination only after the write succeeds.

The main and pause menus both open the same settings screen, which keeps the
screen that opened it as its return destination. Volume, mute and look settings
take effect without rebuilding game state. Display settings go through
`Window::setDisplay` and `Window::setVsync`; a fifteen-second confirmation owns
the previous display values and restores them on cancel, Escape or timeout.
Command-line `--volume` and `--size` remain authoritative for verification
runs.

Settings rows carry their rendered values in fixed-size character storage in
`MenuView`. The renderer therefore draws the screen through the existing HUD
batch without formatting into heap-backed strings. At small resolutions the
eleven rows reduce their height and gap together so the final row remains
inside the panel. At large resolutions the panel, rows and bitmap-font scale
all have matching ceilings, and the settings rows begin below the subtitle, so
1440p and larger framebuffers cannot make text outgrow or overlap the layout.

### Player input actions and rebinding

`src/game/input_actions.h` is the stable vocabulary between devices, settings
and gameplay. Its 25 actions cover the six flight axes, primary and secondary
fire, flare, headlight, automap, four weapon selections, the missile cycle,
chase camera, mute and pause. Each action has a stable JSON name, a
player-facing label and a default keyboard or mouse binding.

The weapon controls are not one key per weapon. The four primaries have a key
each - `1` laser, `2` vulcan, `3` lightning, `4` railgun, in the order a player
reaches for them rather than the order `WeaponId` declares them - and the two
missiles share `G`, which steps to the next one. `kWeaponSelectActions` pairs
each action with the gun it selects rather than indexing a weapon list by the
action's position, so the two orders can differ without a cast quietly
selecting the wrong gun; `--weapons-test` checks the pairing covers every
weapon exactly once, which is what keeps a newly added gun from being
unreachable.

The cycle itself is `stepCombat`'s, not the input layer's: `FireControls`
carries a one-shot `cycleSecondary` and the simulation resolves it against the
selection it holds, through `sim::nextSecondaryWeapon`. A selection that is not
in the cycle at all - a primary, or a corrupt save - lands on the first missile
rather than leaving a gun on the right trigger.

Actions are named for what they select (`selectLightning`) and not for the key
they sit on. A positional name would have made an old settings file's `weapon3`
select a different gun after the keys were reorganised; with weapon-named
actions the superseded entries are simply unknown, so a file written before the
change gets the new defaults. `--settings-test` covers that case. The simulation still receives only
`sim::ShipControls`; `src/game/bindings.h` translates the current bindings at
the edge of the fixed step.

Bindings are part of the versioned player settings document. Each entry stores
a device and code under the action's stable name, so adding an action gives old
files its default and unknown future actions remain harmless. Invalid device
codes normalize to the action default. Assigning an input that another action
already owns swaps the two bindings, keeping every action reachable without a
separate conflict dialog.

The controls screen is a window over the action list rather than a fixed page.
At 1600×900 it shows seven actions plus reset and back; at 640×360 it shows
three plus those two permanent rows. The cursor traverses all 25 actions and
the window follows it. Confirming or clicking an action enters capture mode,
where the next keyboard or mouse press becomes the binding and Escape cancels.
Gameplay shortcuts are suppressed for the capture frame, so binding the old
mute or camera key cannot also trigger that action behind the menu.

### Persistent run history

`src/game/progress.h` stores finished-run history separately from preferences.
Each level record contains finished runs, wins, the fastest winning time and
the highest robot tally. The stable identity is the level filename rather than
its player-facing `name`, so changing display copy does not erase a record.
The progress document uses the same platform configuration directory and
temporary-file replacement discipline as settings; `--progress PATH` redirects
it for isolated runs.

Progress loads before the level list is built. Completed rows carry their best
time in the fixed-capacity button value, while uncompleted levels remain plain
and equally selectable. When a session reaches Won or Lost, the app records it
once before opening the frozen summary, saves the document and updates the
in-memory level row. The summary highlights a first completion, faster time or
higher robot tally. Automated command-line, frame-limited, screenshot, resize
and level-switch runs never write player history.

### Loading a second mine

Until this point the level was loaded once, before the renderer existed, and
nothing could replace it. Switching levels is one function in `main.cpp`,
`loadLevelFile`, and every piece of per-level state is rebuilt there:

- `Renderer::setLevel` rebuilds the level mesh, the mine occluder and the
  static props. Shaders, wall textures, the ship and the robot meshes are
  level-independent and are left alone, and the model caches are keyed by name
  so two mines that both place a pillar load it once. GL resources of the
  previous level are released by the same RAII that built them.
- `LevelSession::load` rebuilds the initial state, the objects, the robots and
  the game flow, and the live and interpolation states with them.
- The frame loop's own caches - the contact log, the automap, the door
  signature that drives the light rebuild, the light editor's selection - are
  reset alongside them. They index segments, lights and doors of the mine that
  was loaded a moment ago; anything left stale here shows up as a light inside
  a wall or a ship inside the rock.

The scratch buffers that are sized from the level (`PortalWalk`, `LightFlood`)
grow on demand and are stamped per build, so a larger mine resizes them and a
smaller one simply uses less of them. Nothing else in the loop holds a pointer
into the level: lights are gathered per frame, and the level is passed to the
renderer by reference on every call rather than stored.

`--level-switch-test` is the check, because a level switch cannot be verified
without doing one: it drives the list down one row, starts that level, and
flies it long enough that the new mine is simulated and drawn rather than
merely loaded, then reports what it started on, what it ended on and whether
the ship is inside it. Its step rate is below 120 by construction - the first
twenty frames are spent in the menu, where there are no simulation steps to
take - so that line is not the check; the switch report is.

---

## The automap (M8)

`Tab`. The segment graph drawn as lines, in the same reversed-Z projection the
scene uses, from an orbit camera the mouse drives. The simulation is untouched
while it is open: the ship keeps flying, robots keep hunting, the countdown keeps
running. Opening the map is not a pause, and reading it while something is
shooting at you is the cost of looking.

It reuses `DebugLines` rather than introducing a mesh path, because that is
exactly what it needs — a per-frame line list that keeps its capacity and stops
allocating after the first few frames. `mapLines_` is a second instance, kept
apart from the scene's `debugLines_` so `--debug-lines` and the map cannot
overwrite each other.

### What it draws, and what it refuses to

The whole mine is drawn from the first frame — the map is the floor plan the
ship launched with, not a memory of where it has been. What the player *has*
done is carried by colour instead: a segment the ship has been inside is drawn
bright, one it has not is drawn dim, and the two read apart at a glance without
either disappearing.

A wall side is drawn as its outline, in the colour of the segment it belongs to.
A portal between two segments in the **same** state — both flown, or both not —
is drawn as nothing at all, so a corridor reads as a gap rather than as a line to
bump into. A portal that **crosses** between the two gets a faint outline
brighter than the unexplored walls around it: that is the frontier, and it is
where the map stops being a record and starts being a plan.

`GameState` still records the knowledge, and for everything that is not the
layout itself the rules stay deliberately mean:

- **`visited`** is segments the ship has actually been inside — not segments it
  has *seen* down a corridor. It no longer gates what is drawn, only how, which
  is the one thing it is honest about: it is a record of where the ship went.
- **`seen`** marks a pickup whose segment has been visited, so a key noticed and
  left behind can be flown back to. Keys are still not given away by the map:
  knowing the shape of a room is not knowing what is in it.
- **`reactorSeen`** needs a real line of sight, tested with the same `raycast`
  the robots use and against the doors as they stand that step, so a reactor
  behind a shut door is not revealed through it. It runs only until it succeeds
  once — a handful of rays per level, not per step.

None of it feeds back into the simulation. A level plays out identically whether
the map is ever opened or not, which is what makes it safe for `stepGame` to
write and for the renderer to read.

### Framing

The orbit camera frames the level's bounding box, which is also what is drawn.
It does not move as the player explores: a frame that grew room by room would
lurch and rescale every time a portal was crossed, and the whole point of
showing the mine from the start is that its shape is a fixed thing to read
against.

The distance is solved rather than tuned — `radius / sin(half-angle)`, the
half-angle being the vertical one on a wide window and the horizontal one on a
tall one, times a little air. The constant it replaces was fitted to the few
rooms the map used to show and leaves a whole mine's far corners a couple of per
cent outside the frustum, which reads as the map being cropped.

`--reveal` now only changes the shading and the markers: it draws every segment
as though flown and every key and the reactor as though found. It is a rendering
check for the map itself — the same family as `--wireframe-volumes` and
`--stencil-view` — and it touches only what the renderer is handed, never
`GameState`.

---

## Feel: flashes, flinches and kicks (M8)

Three effects that exist to be felt rather than read. All three share one rule:
**they land on `RenderState` and `HudData`, never on the body.** A camera effect
that moves the ship is a physics bug wearing a costume, and the tests assert the
ship's position and velocity are untouched by each of them.

### Muzzle flash

Firing spawns a short `Effect` at the fire point, carrying a light. It is scaled
from the weapon's **own** bolt light rather than from a constant — radius x1.5,
intensity x2.5 — so a vulcan pop is small, a missile launch throws real light
down the corridor, and a weapon retuned in `weapons.json` brings its flash with
it.

It lasts 0.05 s rather than literally one frame. At 120 Hz a single step is 8 ms,
which at any real frame rate is as likely to be missed entirely as seen; 0.05 s
is long enough to land on a frame and still be gone before it reads as a lamp
bolted to the nose of the ship.

Robots flash too, from the same `spawnEffect` — which is why that function is
public in `combat.h` rather than file-local. In a black corridor a robot's muzzle
flash is usually what tells the player where it is, before the bolt arrives and
before the eyes are close enough to see.

### The hit flinch

`PlayerState::hurtTimer` is set wherever shields come off — both the direct hit
and the splash path — and decays in `stepCombat`. The renderer turns it into
bands of red at the screen edges, drawn **even when the HUD is hidden**: it is
damage feedback rather than a readout, and a screenshot taken with `--no-ui`
should still show the player being shot at.

Eight bands of falling alpha rather than a gradient shader, because `Hud::rect`
is a flat colour and this is on screen for under half a second. At four bands it
reads as four rectangles, which looks like a bug rather than a flinch.

### The missile kick

`recoilTimer` is set only by concussion and homing, and pushes the **eye**
backwards along the ship's forward axis, squared so it snaps and then eases. A
gun does not kick; the test asserts the laser leaves `recoilTimer` at zero, which
is what stops the effect creeping onto every trigger pull.

---

## Robots

`src/sim/robots.h`. Three types from `assets/config/robots.json`, placed in the
level's `objects` list: **an object whose type names a robot definition becomes
a robot**, and everything else stays static scenery. Robots are `Body`s, so they
cannot leave the mine any more than the player can — no separate containment
logic, and therefore no separate bugs.

### One breadth-first search serves every robot

Pathing is a **distance field**, not a path per robot. One BFS from the player's
segment labels every segment with its distance in portals; a robot steers
towards whichever neighbouring segment has a lower number. Every robot chases
the same thing, so they can all share it, and it is rebuilt only when the player
changes segment.

### Awareness, not line of sight

The state machine is driven by an `awareness` value, not by whether the player
is visible right now. Seeing the player sets it to 1; losing sight decays it
over about three seconds. A robot therefore keeps hunting for a moment after you
break line of sight, instead of forgetting you the instant a wall gets between
you. Being *hit* also sets it — a hit is a noise.

### Darkness hides you, but only until something gives you away

Line of sight is necessary but not sufficient. A robot that has **never seen the
player** cannot pick them out of an unlit stretch of mine; one that is **already
on to them** keeps hunting whether or not the player is standing in the light:

```cpp
const bool alreadyOnToThem = robot.awareness > 0.0f;
if (canSee && (visibility.visible || alreadyOnToThem)) { robot.awareness = 1.0f; ... }
```

That asymmetry is the whole mechanic, and it falls out of `awareness` already
being a decaying value rather than a boolean. Going dark does not call a robot
off. You have to break line of sight and wait the three seconds out; once you
have, the dark hides you from it again exactly as it did before it ever saw you.

`canSee` itself is untouched — still pure line of sight — because the firing and
mode-transition checks downstream are about an engaged robot and must keep
working in the dark.

Three things give the position away, and **none of them is a special case**:

| What | Why it works |
| --- | --- |
| The headlight | `playerVisibility` counts it as a light the player carries |
| Firing | The muzzle flash is a light at the player's own position |
| Hitting a robot | `stepCombat` already sets that robot's awareness on damage |

So there is no sniping out of the dark, and no rule had to be written saying so.

The query is asked **once per step**, not once per robot: it is a property of
where the player is standing, identical for every robot looking at them. The
same answer is what the HUD's darkness gauge draws, so the readout and the
robots can never disagree; see *The darkness gauge*.

### Retreat is what sells it

Pathing is easy and Attack is obvious. What makes robots read as cautious rather
than as turrets is that an unaggressive one, having just finished a burst, backs
off around the corner, waits, and comes back. `aggression` is the dice roll on
that; the brute's is 0.95, so it effectively never does, and it is the wall you
run into.

Aim leads the player's velocity by `lead` — below 1, so a moving player is still
hard to hit, which is what makes strafing worth doing. Early shots in a burst
get extra spread, so a burst walks onto the target instead of all landing or all
missing.

### Friendly fire

A robot's fire never hits a robot, and the player's never hits the player.
Splash damage is the exception and hurts everything in range including the
player, because splash that cannot hurt the firer is a missile you can safely
shoot at your own feet.

### Eyes

Emissive billboards at local offsets from `robots.json`, drawn in the unlit
pass, so they read in total darkness. In a mine with no ambient light the eyes
are usually the first thing the player sees of a robot — which is the whole
reason they exist.

They are billboards rather than mesh faces because the meshes carry no material
split, and a glow reads better at distance than a lit polygon would. A
deviation from the plan's "emissive eye geometry", and worth knowing about.

---

## Measurement (M8)

Two instruments, because M8 says to measure rather than reason, and because the
two questions have different shapes: "where does the frame go" is an
investigation, and "did anything allocate" is an invariant.

### GPU timing

`src/render/gpu_timer.h`. Every timed region is a pair of `glQueryCounter`
calls, read back **two frames later**. Reading them in the frame they were
issued stalls the pipeline waiting for the GPU, which makes the thing being
measured slower than it is — the classic way to profile a renderer into a shape
that does not exist. Three sets of queries rotate; a frame whose results are not
ready yet is skipped rather than waited on, and the last good numbers stay on
screen.

Timed: the depth prepass, the unlit pass, and — separately — the stencil pass
and the lighting pass of each light, up to sixteen. The automap counts as the
unlit pass, which is what it is.

The numbers appear in the `gpu timings` ImGui panel and on a line at exit, so a
headless run reports them the way everything else in this project is reported
rather than only existing behind a panel nobody can open without a display.

### Allocation counting

`src/core/alloc_count.h`. Global `operator new` is replaced with one that
increments a counter. That is a big hammer for a small question and it is the
only one that answers it honestly: it sees allocations from anywhere, including
inside the standard library and inside a container that grew when nobody
expected it to. A counter threaded through the project's own allocations would
only ever find the ones already known about.

`main` brackets the sim-step loop and the `beginFrame`-to-`resolve` span and
reports both at exit. The first thirty frames are exempt, because pools and
vectors reserve on their way to steady state and counting that warm-up would
report a leak that is really a start-up cost.

What it found, which is the point of having it: the steady state really is zero
on both paths, and the exceptions are all **events** rather than churn — the
death message being formatted inside `stepGame`, the render target's log line
when a resize rebuilds it, a per-frame container reaching a new high-water mark.
None of those happen every frame, which is what the invariant actually claims.

### What is deliberately not capped

M8's plan suggested capping per-light volume triangle counts. That is not done,
and `Stats::worstVolumeTriangles` reports the number instead.

A truncated shadow volume is an **open** volume, and depth-fail counting against
an open volume is exactly the bug the shadow-closure work fixed: the earlier
segment-subset optimization left holes in emitted volumes and produced black
polygons behind receivers. Capping would reintroduce that class of failure to
buy performance nobody has yet shown is needed. The stat says how big the
problem would be and which light has it, so a cap can be sized from evidence if
evidence ever appears.

The cheap part of that suggestion was already in place: `buildShadowVolumes`
skips a light entirely when no segment in its set is visible, before any
silhouette work, and `gatherLights` orders the shadow budget by the same test.

---

## Asset validation at startup

`src/game/asset_check.h`. Every `.obj` under `assets/models` and every `.json`
under `assets/levels` goes through the M2 and M3 validators on every run, one
report line each, before a window exists.

Both validators already ran at load — but only for the assets a given run
happens to touch. Play `mine01` and nothing ever looks at `test01`; a model no
level places is never loaded at all. Walking the directories instead means a
broken asset is found on the next run rather than on the run that first needs
it, and before anything is on screen.

Closure is asked through `checkClosure`, which reports rather than aborts, and
which deliberately uses the same weld and adjacency rules as `buildOccluder` — a
report using a different rule could pass an asset the loader then aborts on,
which is worse than no report.

### What "skip it in Release" does and does not mean

M8 asks for a failing asset to be fatal in Debug and skipped with a warning in
Release. That is what a failing **model** does: the game is better off missing a
pillar than refusing to start.

A failing **level** is fatal in both, which is a deliberate departure. Skipping
it leaves nothing to play, so "continue without it" is not a better outcome than
stopping and saying why. This is the same rule the level loader already had.

The level report also prints the surface's Euler characteristic. `V - E + F = 2`
is a closed genus-0 surface, which is the property the mine's own shadow volume
depends on; both shipped levels report 2.

---

## GL debugging

`src/render/debug.h`. `KHR_debug` callback, installed synchronously so an abort
stack points at the offending call.

In Debug builds any `GL_DEBUG_SEVERITY_HIGH` message calls `log::fatal` and
aborts. This is the project's equivalent of treating a Vulkan validation error
as fatal: **no milestone is complete with one outstanding.** Release installs
the callback but only logs.

Notification-severity messages are dropped and NVIDIA's id 131185 ("buffer will
use video memory") is filtered explicitly; both are driver chatter.

---

## Adding a shader

1. Write `shaders/<name>.vert` and `.frag`, GLSL `#version 460 core`.
2. They are copied next to the executable by a post-build step; no compile step
   is needed, the driver compiles at load.
3. Construct a `gl::Shader("shaders/<name>.vert", "shaders/<name>.frag")`.
   Compile and link failures are fatal and print the driver's info log.

Uniform conventions: the camera is a UBO at binding 0 (`std140`, view then
projection). Per-object data is a plain uniform — GL's nearest equivalent to a
push constant. Textures use explicit `layout(binding = N)`.

---

## Verification harness

`--frames`, `--screenshot` and `--resize-test` exist so that a milestone can be
demonstrated rather than asserted. See CLAUDE.md for the flags.

Two things the harness learned the hard way, both worth preserving:

- The timing report excludes the final screenshot encode (~200 ms for a
  1600x900 PNG) and any time spent minimized. Including either makes the step
  rate understate by 5% or more and turns a correct sim into a suspicious one.
- `--resize-test` is keyed to wall-clock seconds, **not** frame count. No frames
  are drawn while the window is iconified, so a frame-keyed schedule never
  reaches its own restore event and the run hangs minimized forever. It also
  ends on time rather than on frame count, for the same reason.

---

## Sound

M9. `src/audio`. Positional sound effects, transmission voices and adaptive
music share one lock-free mixer.

### The rule

`src/sim` may not touch OpenGL. From M9 it may not touch audio either, and for
the same reason: the simulation is the thing every headless test drives, and a
dependency on a subsystem that needs a device would make half of them
unrunnable.

So the simulation does not play sounds. It **reports** them, into a
`sim::EventQueue` (`src/sim/events.h`) — a fixed-capacity array of
`{cue, position, segment, gain}` handed to `stepCombat`, `stepRobots` and
`stepGame` as a trailing out-parameter that is always allowed to be null. The
app layer drains it once a frame and hands it to `audio::Audio`. Nothing under
`src/sim` includes anything under `src/audio`, and `--weapons-test`,
`--robots-test` and `--loop-test` pass null and are unchanged.

The queue is **not** part of `SimState`. `SimState` is copied every step and
interpolated between two copies; an event is neither copyable state nor
interpolatable — it happened once, at one step, and is consumed once. Putting
it there would have meant deciding what `interpolate()` does with half an
explosion.

### The thread, and why the mixer is ours

miniaudio hands the program a callback on a thread it owns, under a hard
deadline. Return late and the driver replays whatever was in the buffer, which
is an audible click rather than a dropped frame. Three things follow, and they
are the whole shape of this subsystem:

- **The render path allocates nothing.** Voices are a fixed array, so is the
  command queue. `--sound-test` checks it with the same `alloc::Scope` counter
  `main.cpp` uses on the sim step.
- **It takes no lock.** The game thread and the audio thread share one
  single-producer/single-consumer ring of POD commands (`Ring` in `mixer.h`).
  The game thread writes only the head, the audio thread only the tail. A mutex
  here would mean the audio thread waiting on the game thread, which is exactly
  the stall that cannot be afforded.
- **Everything except opening the device is ours.** miniaudio has a perfectly
  good high-level engine with 3D sound built in, and it is not used. The mixing,
  the attenuation and the panning are about 200 lines in `mixer.cpp`, and
  writing them bought a `Mixer` with no dependency on miniaudio, on GLFW or on a
  sound card existing — which is what makes `--sound-test` possible. The same
  trade as `render/gl.h`: one file knows about the platform library, and the
  interesting code is testable because it does not.

### Positioning

Every sound is a point. Distance gives inverse-distance attenuation from a
reference radius of 9 units, faded linearly to nothing at 220 — the fade
matters, because an inverse curve alone never reaches zero, so a sound ten
rooms away would still cost a voice and still contribute a hiss.

Panning is equal-power, and is taken against **the ship's own right vector**,
not world X. In a 6DOF game that is not a detail: fly upside down and a sound on
your left has to move to your right, or the audio contradicts the view.
`--sound-test` checks it by rolling the listener 180° and asserting the image
inverts.

Two exceptions, both deliberate:

- **Sounds the player makes play at the listener**, not in the world —
  `SoundEvent::atListener`. The laser fires from a wingtip half a metre off the
  camera, and panning by that offset puts the player's own gun hard right, which
  is wrong and obviously wrong. The same goes for their own hull being hit, and
  for the reactor, which is the whole mine rather than a point in it.
- **Very close sounds collapse to centre.** Without it a bolt spawned at a
  wingtip pans hard to that side for one frame and clicks across as it leaves.

The listener is the **ship**, not the camera. In chase view the camera sits
several units behind the hull; hearing from back there would put the player's
own guns slightly in front of them. The chase camera is a view of the ship, not
where the pilot is.

### Voices

32, fixed. A full pool steals the quietest voice rather than dropping the newest
request — a missing gunshot is more noticeable than a quieter one being cut. An
ambient channel is never stolen: losing the engine loop to a burst of gunfire
and never getting it back is worse than losing a shot.

The bus is soft-clipped with `tanh` rather than clamped. With a dozen explosions
in one room the sum genuinely passes 1.0, and a clamp turns that into buzzing
square edges.

### Ambient channels

Named rather than handles: `Engine`, `Alarm`, `Comm`, `MusicLull` and
`MusicCombat`. They are protected from voice stealing, and re-setting a channel
updates the running voice rather than restarting it. That keeps the engine from
stuttering with every throttle change and lets both music transports keep their
place through a crossfade.

The drive is driven by the ship's **speed**, not by the throttle key — a ship
coasting with the throttle released still sounds like it is moving, which is
what a player watching the walls go past expects.

**There is no idle hum.** Below half a unit per second the channel is stopped
outright. M9 shipped an idle gain of 0.16 and it was wrong: a parked ship
droning away is one more sound competing with the mine, and the drive is meant
to be information about motion. The ramp is measured from that threshold rather
than from zero, so the drive fades in from silence as the ship moves off; the
restart therefore happens at zero gain and cannot be heard, which is what makes
stopping the channel safe rather than a click every time the ship comes to rest.

The gain at top speed is 0.275, half what M9 shipped. At 0.55 the engine sat on
top of the guns instead of under them.

Loops must be seamless. `engine.wav` and `alarm.wav` are built with every
partial at an integer number of cycles over the loop length and the tail
crossfaded over the head (`seamless()` in `tools/make-sounds.py`); without that,
a one-second loop clicks once a second.

### Adaptive music

`Sub-Bass Drones.wav` and `Mechanical Devastation.wav` start together when a
level starts and remain sample-aligned through play and pause. Zero gain does
not stop these two channels: transport and audibility are separate for music,
so bringing the combat track back cannot restart it at its introduction. Music
stops in the main menu and run summary.

The game layer takes the maximum awareness of every living, enabled robot as a
0..1 combat value. Awareness snaps to one when a robot acquires the player and
decays for about three seconds after contact breaks, so the AI's existing memory
is also the soundtrack's hysteresis. Player damage and the reactor countdown
force combat to one.

Awareness dies with the robot holding it, though, and hysteresis that lives on
the robots cannot survive the last one of them. So the game layer holds the
combat value for five seconds after combat stops: anything at or above 0.5
refreshes the hold, and only five uninterrupted seconds start the return to the
lull. A robot waking up inside that window resets the hold without a transition
ever having begun. The hold sits at the level combat actually reached rather
than at one, so a robot that half noticed the player does not buy five seconds
of full combat music. The hold is frozen while the game is paused, a pause not
being five seconds of not fighting.

`Audio::setMusicState` takes that value as a target rather than a level and
holds the envelope that walks towards it: about 1.5 seconds into combat and
about 4 seconds back out, so the two tracks cross rather than swap. The
envelope is linear and is stepped by the frame delta the caller passes, which
is zero while paused. Its position feeds an equal-power pair - the lull at
`cos(k*pi/2)` and combat at `sin(k*pi/2)` - so the two are at about 0.71 each
halfway through and the crossfade has no hole in the middle. A level that opens
in combat opens in combat: the first call after the music starts snaps rather
than fading in from a lull the player was never in.

The envelope is deliberately not the mixer's own gain glide. That glide is a
one-pole, which only ever approaches its target, and the voice's glide is
overwritten by every following command - and this state is re-sent every frame,
so a four second music fade used to become the anti-zipper default one frame in
and read as an instant switch. The mixer is now asked only to keep a frame's
step from clicking.

A scripted transmission multiplies both music gains by 0.22, on its own
envelope over the top of the crossfade, with a short attack and a longer
release. It does not touch master gain, effects or the transmission voice. The
fixed `--comm` screenshot panel is deliberately excluded because it is not a
live transmission and plays no voice.

The WAV loader retains stereo for two-channel sources and keeps mono for
positional effects. Non-positional stereo voices go straight to left and right;
if a stereo clip is ever played positionally it is downmixed before panning.
Both long tracks are decoded into memory at startup, trading memory for the same
allocation-free callback and simple seek-free looping as every other clip.

### Where the WAVs come from

`assets/sounds/*.wav`, 26 files, mono 16-bit 44.1 kHz, written by
`tools/make-sounds.py` — committed, deterministic, stdlib-only, and
regenerable. They are synthesised from oscillators and filtered noise, in the
same spirit as `render/procedural.cpp` generating the textures.

They are ordinary WAV files with no special status. `wav.cpp` reads PCM
8/16/24/32 and float at any rate and any channel count, so any of them can be
replaced with a recording or a downloaded effect without touching code. The
**names** are the contract — `kCues` in `audio.cpp` — not the contents.

### A finding: the null backend

miniaudio is compiled with `MA_NO_NULL`. Its null backend is tried last and
always succeeds, so leaving it in means a container with no sound card opening a
"device" successfully and the game reporting sound it is not producing. Without
it, such a machine fails to open, `Audio::init` warns, and the run says
`device "none"` — which is true.

## Combat, force and things that move

Added after M8, in four phases. Each is its own commit.

### One trace, not three

`sim::traceCombatRay` answers "what does this shot reach first" — the nearest
of wall, shut door, robot, destructible object, player and floating light,
walking the segment graph as it goes. The projectile loop had grown three
near-identical copies of the same closest-point-on-segment test, each compared
only against the wall and never against each other, so a bolt that reached a
robot and a crate in the same step took the robot regardless of which was
nearer.

Two enums that look parallel are kept apart by functions rather than casts.
`weaponSlot()` decides primary or secondary; `weaponFire()` names the delivery
mode; `projectileFor()` maps a weapon to the bolt it launches. The last one
replaced `static_cast<ProjectileKind>(weaponId)`, which was correct only while
`ProjectileKind::Flare` at index 4 happened to sit past the last weapon. All
three are switches with no default, so the compiler names the next weapon that
forgets one — which it did, one commit later.

### Knockback is a second velocity channel

`Body::knockback` is separate from `Body::velocity` because everything that
owns `velocity` would erase it. `stepShip` damps velocity toward zero and then
clamps it to `maxSpeed`, so a shove above cruising speed would be discarded in
the step it arrived in; `steer()` drives a robot's velocity toward whatever it
means to be doing. Neither gets a say in the new channel. It decays at
`kKnockbackDamping`, deliberately independent of `linearDamping`, so retuning
how far the ship coasts does not change how a blast feels.

Every integration site moves at `Body::motion()`, and `resolveCollision` takes
the inward part out of both channels — being blown into a wall is not a way
through it.

`applySplash` casts from the blast to each target before touching it. It used
to check distance alone, so a grenade on the far side of a wall damaged
straight through. Damage falls off linearly; the shove falls off squared, so
the rim of a blast rocks you and the middle of one throws you.

`CollisionConfig::restitution` defaults to zero, which is the sliding response
every body has always had. Above zero it reflects. Two hazards come with it:
the iterated push-out can compound a bounce in a corner, so nothing leaves a
collision faster than it arrived; and `scrapeDamping` would bleed a bounce away
as fast as it is made, so anything bouncy sets it to zero.

### Hitscan weapons

A railgun modelled as a very fast bolt would still be a bolt: swept step by
step, and still able to tunnel if it were fast enough. `WeaponFire` names the
three modes structurally. `Instant` traces once per trigger pull; `Continuous`
traces every step the trigger is held, with damage, energy and impulse read as
per-second rates multiplied by `dt`.

The lightning gun traces 120 times a second and must not be heard or seen to
flash 120 times a second, so its cue, muzzle flash and impact light are gated
to one per `fireInterval` while the beam itself is drawn every step.

Beams are quads turned about their own axis to face the viewer, built into the
same buffer as the sprites so the unlit pass keeps to one draw. Looking straight
down a beam the side vector degenerates; the camera's own right vector is the
stable answer there, because a beam pointed at the eye must stay visible rather
than vanish or flip. `RenderState` does not interpolate them: a beam is struck
at one instant and lives a tenth of a second, so blending it between steps
would smear it.

### Hand-drawn pickup sprites

Most of what the unlit pass draws is a glow, and a glow is cheaper to compute
than to store: `billboard.frag` makes a squared radial falloff out of the quad's
own UVs and no texture is involved. Pickups are the exception, because a health
cross and a key are shapes rather than blobs, and no falloff function is going
to produce them.

`render/sprites.h` is the counterpart to `render/procedural.h`: that file makes
tiles because nobody drew any, this one reads the ones somebody did. The three
images in `assets/images/` become the layers of a second `TextureArray`, and a
pickup kind names a layer. The health and energy art carries its own colour and
is tinted white; the key is a white template, and blue, yellow and red keys
differ by nothing but the tint applied to it. The four kinds with no art —
laser, concussion, homing, flares — still take the procedural path, so both
kinds of quad coexist.

They coexist in one draw call, which is the constraint the design is built
around. The layer index rides in the vertex alongside the colour and the UV, and
a negative layer is the sentinel for "no texture, use the falloff" — the same
trick `hud.frag` plays with a negative UV to keep bars and glyphs in one draw. A
texture array rather than an atlas because the layers are same-sized squares
already, so there are no atlas seams for the mip chain to bleed across.

Two things about the pixels. They are **premultiplied** at load, because the
pass is additive (`GL_ONE, GL_ONE`) and additive blending never looks at alpha:
the fade at a sprite's edge has to be carried in the colour or it is not carried
at all. Premultiplying also fixes the dark fringe that averaging straight alpha
produces — both in the load-time box filter and again in the mip chain the GPU
builds — because a transparent pixel's RGB is black and would otherwise be
averaged in as if it were visible. And they are **resampled to 256** to match
`kWallTileSize`: the art is 1254 square, which is twenty-four times the pixels a
quad a hundred across on screen can show, and the mip chain would carry all of
them.

`stbi_set_flip_vertically_on_load` is the mirror of the
`stbi_flip_vertically_on_write` in `screenshot.cpp`, and for the same reason: a
PNG's first row is its top and a GL texture's first row is its bottom. Nothing
else in the project reads an image file, so the global flag has no other caller
to surprise — the comment in `sprites.cpp` says so, because the next image
loader will need to know.

### Door signs

A locked door and an unlocked one were the same striped panel. The only feedback
for the difference was flying into it and hearing a refusal, which is a slow way
to learn a mine's layout. Each keyed door now carries two lit plates: a status
strip reading `ACCESS DENIED` or `ACCESS GRANTED` depending on whether the player
is carrying the key, and under it a plate in the key's own colour reading
`BLUE KEY REQUIRED`, `YELLOW KEY REQUIRED` or `RED KEY REQUIRED`. The second
plate does not change with the first: which key the door wants is a property of
the door, and a player who has just been refused needs to know what to go and
find.

They are **emissive**, drawn in the unlit pass rather than lit by the mine. That
is forced by the no-ambient rule: a sign painted into the lit pass is invisible
in an unlit corridor, which is exactly where a player is most likely to be
guessing about a door. An illuminated sign is what a mine would have anyway.

`render/signs.h` is the world-space sibling of `render/hud.h` and is built the
same way — a bezel quad, a plate quad and one quad per letter, all in one stream
buffer and one draw. It reuses `hud.frag` **unchanged**, including the negative-UV
sentinel that lets solid quads and glyphs share a draw call; the only new shader
is `sign.vert`, which differs from `hud.vert` in nothing but putting the quads
through the camera instead of through the screen. The 5x7 font both of them draw
with moved out of `hud.cpp` into `render/font.h` to make that sharing possible.

Nothing inside the buffer is depth-offset from anything else. The pass writes no
depth, so the layering is simply the order the quads were emitted in, and the
only offset that matters is the 0.02 units that hold the whole sign clear of the
slab face it is stuck to. The copy of the sign on the door's far face needs no
special handling either: it is behind the slab, and the slab's own depth from
the prepass rejects it.

The one piece of real work is the seam. A door is two halves that slide apart,
so a sign across the doorway has to come apart with them. `DoorSigns::Panel`
describes the whole sign but carries a horizontal slice `[uMin, uMax]` of it,
and each half asks for its own slice positioned at its own centre. The lettering
is laid out across the **whole** doorway and then clipped per glyph, adjusting
the glyph's UVs as well as its geometry, so a letter straddling the join is cut
down the middle and each piece rides away on its own slab rather than being
drawn twice or squeezed to fit. Seen from behind the door, the half that carries
the first slice swaps over, because the door's left-hand half is on that
reader's right.

`RenderState` gained a `keys` array for this. `HudData` already had the same
three flags, but those are a readout; a locked door's signage is part of the
world, and the renderer has to know what the door would do if the player flew at
it. It is not interpolated — picking a key up happens on one step, and half a
key is not something a door can be told about.

The slab and the sign now share one `doorFrame()`, which derives the opening's
`across`, `up` and `normal` and the slab's half-extent and travel from the four
corners. They were computing the same thing twice, and a sign that disagreed
with its door about where the doorway is would be visibly wrong.

### Disabled ships

One threshold, `kDisabledHealthFraction`, governs robots and the player alike.
A disabled ship keeps exactly the velocity it had — no damping, no speed clamp,
and the knockback channel is folded into velocity rather than left to decay,
because with nobody flying the ship a shove and a drift are the same thing.

For robots this is `RobotMode::Disabled`, entered ahead of every other
transition. It is expressed as "skip everything else" rather than as a desired
velocity of zero, because `steer()` reads zero as a request to stop and would
brake the robot against the very momentum it is supposed to keep.

The red is emissive first. `wall.frag` gained `uAlbedoTint`, but this game has
no ambient term, so a tint only shows where light already falls — and a robot
drifting in an unlit corridor gets none. The cue that carries the information is
the eye billboards, pulsing red and dim. The player, who cannot see their own
hull in first person, gets the damage vignette held on and pulsing instead of
fading: the flinch is an event, this is a state.

### Props that move

`sim::PropState` is simulation state for things that move but are not ships,
separate from `Level::objects`, which is authoring data and assumes nothing
ever leaves the position it was placed at.

A floating light is a `Body` with restitution above zero and no scrape, so it
bounces. It takes no damage — shooting one moves it — but it stops a shot,
because you can see it. A swinging light is analytic: one sine, from which the
lamp position, the spot direction and the cord's lower end all follow. A
constraint solver would be the wrong tool; the cord holds to five decimal
places by construction. Both are dynamic lights rebuilt every frame on the same
path the bolts take, which is what makes a swinging spot sweep real stencil
shadows across the room it hangs in.

Their visible fixtures are emissive billboards. They do not cast shadows
themselves: `Renderer::Prop` caches its model matrix and occluder once at load,
and a moving fixture would need the per-frame `setTransform` treatment robots
get. The light moves and sweeps shadows; the housing does not cast one.

### A finding: findSegment is not a containment test

`--world-test` reported floating lights escaping the mine. They were not.
`findSegment()` asks which hexahedron holds a point and answers -1 on a point
sitting exactly on a shared face, which is inside the mine but not
unambiguously inside any one segment. `insideMine()` — ray parity against every
wall triangle — is the containment question, and it now lives in `level.cpp`
with one definition instead of a private copy in `collision_test.cpp`.

The wrong test also made the fix look wrong: sub-stepping the ball's
integration appeared not to help, then appeared to make things worse. Under the
right test, 150 u/s shoves from every floating light in all 26 directions never
leave the mine without any sub-stepping at all, so that code came back out.

## The comm panel is presentation, not simulation

*(Historical: there is no longer a level-opening transmission at all — see
"Transmissions are level content" below. The clock described here is unchanged.)*

The level-opening transmission used to be `sim::CommState` inside
`sim::GameState`, advanced by `stepGame` and retired against
`GameConfig::commSeconds`. Nothing in the simulation ever read it back: the
only consumer was the HUD. It was a playback cursor living inside the thing the
simulation is, in the same category as the automap's `visited`/`seen` arrays
but weaker still — those are at least knowledge the player has acquired.

`src/game/comm_player.h` owns it now. `CommPlayer` holds the clip's timing, a
start stamp and nothing else, and `at(now)` answers which frame to draw and how
far into its fade it is. `HudData` carries `commFrame` and `commAlpha` instead
of `commTime`; the renderer draws what it is handed and no longer owns any part
of the clip's clock. `sim::GameState` and `GameConfig` lost their comm fields
entirely.

### Why an epoch and not a delta

The clock is `now - startSeconds_`, never an accumulated per-frame delta, and
that is the whole reason the type exists rather than a free float somewhere.

The main loop clamps its frame delta to `kMaxFrameTime` before banking it, and
drops it altogether while the window is minimized — both deliberate, so the
simulation cannot bank time it did not simulate. Anything that adds those
deltas up inherits that: a frame that overran the clamp, or a spell behind
another window, leaves a gap it never makes up. For the simulation that is
correct. For a transmission meant to run beside a voice on the audio device's
clock it is drift with no ceiling, and the obvious wiring —
`comm.advance(elapsed)` in the loop body — picks up the clamped value silently,
because the clamp reassigns the variable it clamps. Subtracting a start stamp
cannot drift however sparse the samples are, and `--comm-test` asserts exactly
that: a jump straight to t=3.0 lands on the same frame as stepping there.

`--comm` still pins a repeatable frame for screenshots, so determinism comes
from the flag, which is where it came from before. The clip itself is still
authored PNG frames in a texture array; only the clock moved.

### Nothing pauses

A transmission keeps running behind the pause menu. That is a decision, not an
oversight. `Mixer` has no pause and no way to address a voice once it has
started, so pausing the picture alone would leave a voice talking over a frozen
frame. The HUD renders while paused anyway. A level switch stops the player, so
the outgoing level's transmission does not carry into the incoming one.

## A finding: an ambient channel was not protected

`Mixer::claimSlot` promised in its own comment that an ambient channel is never
the voice stolen when the pool is full. It was not true, for two reasons in the
same four lines.

`quietest` was seeded to slot 0 and `quietestGain` to `0.0f`, with the first
candidate taken on `i == 0`. When slot 0 held an ambient voice the loop skipped
it without ever seeding, every later comparison ran `current < 0.0f` against a
gain no real voice is below, and the function returned slot 0 — the one slot it
had just decided not to touch. Slot 0 is where the engine loop lands, because
it is the first sound a level starts with a quiet pool.

The second is `Voice::gainL`, which is `-1` until the voice has been through a
block. Read as a level, `-1` is quieter than anything, so a voice started
earlier in the same `drain()` was the preferred victim: a burst arriving
together cut its own earliest and loudest member and kept the near-silent ones
behind it. `claimSlot` now asks `spatialise` what an unmixed voice is about to
be mixed at, which is the number it meant to compare all along.

The engine survived this in practice because `setAmbient` is pushed every
frame and a stolen channel releases its slot, so the next frame restarts it —
an audible cursor reset rather than silence, which is why it went unnoticed. A
voice started once would simply die, which is what makes it worth fixing before
anything is built on the guarantee.

`claimSlot` can now return -1, when every slot is a protected channel and
there is nothing left to cut. `start()` drops the sound, the way a full command
queue drops its newest push. Unreachable while there are fewer ambient channels
than voices.

### Protection and looping are separate questions

`SetAmbient` forced `v.loop = true`, so taking a channel for its protection also
meant taking its loop. A voice that has to survive a firefight but still end by
itself — a spoken transmission, for instance — could not be expressed. `Command`
carries `loop` now, defaulting to true so the engine and the klaxon are
unchanged. No channel uses the non-looping form yet; `--sound-test` drives it.

## Lua level scripts

A level may name a script: `"script": "mine01.lua"` in the level JSON, resolved
at load against the level file's own directory. `loadLevel` refuses an absolute
path and refuses one that climbs out of that directory, and `saveLevel` writes
it back relative or refuses to write at all. A level with no `script` key has an
empty `Level::scriptPath` and no Lua state is ever created for it.

`src/game/script.h` owns the one state. The rule it exists to keep is the one in
CLAUDE.md: **no Lua object crosses into `sim` or `render`**. Lua executes in
exactly two places, both in the game layer:

- once at load, running the file to collect its `message` and `trigger`
  declarations into POD, and
- once per **rendered** frame, in `drain()`, after the accumulator has finished
  stepping.

What the fixed step sees is `sim::TriggerSet` — an authored array of volumes —
and what it produces is `sim::TriggerState`, plain arrays of bytes that copy
with the rest of `SimState`. `stepTriggers` is a standalone sim function so
`--script-test` can prove enter/re-enter behaviour without building a game
around it. Every step latches trigger ids; `drain()` runs the callbacks
afterwards. That ordering is deliberate and is why Lua may allocate: it is
outside the measured step, so a callback cannot compromise sim determinism or
show up in the sim-step allocation report.

`latched` deduplicates the pending list, so however many fixed steps fall
between two rendered frames, a trigger enqueues at most once and its callback
runs at most once per frame.

### The sandbox is a budget, not a promise

Level scripts are content, and content is the thing most likely to be wrong.
The state gets base, `math`, `string` and `table` and nothing else — no `io`,
no `os`, no `package`. `dofile`, `load`, `loadfile` and `collectgarbage` are
removed from the globals afterwards, so a script cannot reach back out through
the loader or stall the frame in a collection it asked for.

Two hard limits back that up. The allocator is the host's own, counting against
an 8 MB budget and returning null past it, so a runaway table fails the
allocation rather than the machine. A count hook fires every 100,000 VM
instructions and errors out, so a `while true do end` in a trigger callback
ends that callback instead of the process. Both are armed around the load call
and around each callback, and disarmed in between.

Declarations are bounded too: 64 triggers per level, and message ids, clip
names, speakers and body text are all length-capped at declaration. The HUD
relies on that last one — `Hud` reserves its worst-case vertex count up front
precisely because the longest message a level may declare is known at load, and
the draw path must not grow a vector.

## One comm library, one texture array

`assets/comm` is a directory of clips, not a single clip: every subdirectory
holding a `comm.json` is one, and its directory name is the name scripts use.
`loadCommClips` walks them in sorted order — sorted so the layer assignment is
the same on every machine and every run — and concatenates every frame into one
`CommClipLibrary`. Each `CommClipInfo` keeps the `baseLayer` where its own
frames start.

That concatenation is the whole design. All the frames become layers of a
single texture array, which is why every clip must share one width and height
and why `loadCommClips` is fatal when one does not; the layer count is checked
against `GL_MAX_ARRAY_TEXTURE_LAYERS` at construction rather than discovered as
a silently truncated upload. One array means selecting a transmission never
rebinds a texture and never decodes anything: it changes which layer the HUD
quad samples and nothing else.

Selection is split across the three layers the way everything else here is.
`HudData` carries `commClip` and `commFrame`; `commFrame` counts **within** the
clip, and the renderer resolves `clip.baseLayer + commFrame` on its own. The
game layer holds the clip index beside the `CommPlayer` that produces the frame
index, because the two are meaningless apart — a frame index against the wrong
clip is another transmission's pictures. `CommPlayer` is configured from that
clip's `frameCount` and `framesPerSecond`, so a clip shorter than its message
loops rather than freezing on its last frame.

Names are resolved once, at load, and never at playback: `Renderer::commClipIndex`
answers a name, `Renderer::commClip` answers an index, and both are checked at
startup. Every clip a level's Lua names, and any name given to `--comm`, is
validated before the first frame — an unknown one is fatal there, with the
message id that named it, rather than a blank panel ten minutes into a mine.
Resolution at the draw call clamps instead of trusting, so a stale index left
over from a level switch cannot sample past the end of the array.

### The merge that lost the selection

The library landed alongside the move of the comm panel out of the simulation,
and the two merged without a textual conflict while contradicting each other.
`Renderer` had kept three whole-clip accessors — `commFrameCount()`,
`commFramesPerSecond()`, `commDisplaySeconds()` — reading members the library
change had deleted. That much was a compile error and could not survive.

What would have survived is the rest of it. `HudData::commClip` was added and
never written by anyone; the draw call went on treating `commFrame` as an
absolute layer; `CommPlayer` was configured with the *library's* total frame
count and the *first* clip's rate and duration. With one clip shipped, all of
that is indistinguishable from correct. It is wrong the moment a second clip
exists, and it is wrong silently: `--comm NAME` validated the name and then
played clip zero, and a Lua message's `clip` field was parsed, length-checked,
validated against the library — and then never read again.

A clean automatic merge is not evidence of a correct one. The check that
catches this class of thing is a second clip in the library and a screenshot of
each, which is what `--comm NAME` is for.

## The comm panel decodes video

A comm clip is now either an authored frame sequence or an MPEG-1 video, and
`CommClipLibrary` loads both so a set can be converted one clip at a time.

The reason is that the frame-sequence design costs `width * height * 4` bytes of
texture for every frame of every clip, resident from startup, and the same
frames as PNGs in the repository. Both grow with the total authored seconds
across the whole library — which stopped being hypothetical when clips became
named and script-triggerable, because that is the point at which anyone would
start adding them. `testcard` is 20 seconds at 24 fps: 480 frames, about 39 MB
of PNGs and 126 MB of texture with mipmaps. Decoded it is 544 KiB on disk and
**one** texture layer, and a longer clip costs neither more.

`src/render/comm_video.cpp` is the only file that includes pl_mpeg and the only
one that defines `PL_MPEG_IMPLEMENTATION`; `comm_video.h` hands back RGBA and
nothing else. That is the containment `device.cpp` gives miniaudio.

### One layer, rewritten

A decoded clip owns a single scratch layer in the same texture array the
authored sequences live in, rewritten as its picture changes, so the draw path
is the one it always was: compute a layer, call `hud_.image`. `CommVideo::frame`
returns null when the requested frame is the one already showing, which on a
120 Hz loop drawing a 24 fps clip is four draws in five.

`TextureArray::updateLayer` regenerates the whole array's mipmaps, because GL
has no per-layer form of it and the HUD samples this array minified. The cost is
proportional to the array's total layers rather than to the one that changed,
and it is paid when a video frame changes rather than once a draw. With a
handful of clips that is nothing; it is the thing to revisit if a library ever
holds enough authored sequences for the sweep to matter.

Playing forward is a decode and anything else is an exact seek, which decodes
from the preceding intra frame — so `make-comm-video.py` pins a short GOP, and
`--comm-test` asserts that sequential playback takes the cheap path rather than
seeking its way through. Decoding allocates nothing after `open()`: pl_mpeg
takes its buffers up front and the RGBA target is allocated beside them, which
`--comm-test` measures with the same counter `--sound-test` uses on the mixer.

### A finding: plm_frame_to_rgba does not write alpha

It writes the three colour bytes of each pixel and leaves the fourth exactly as
it found it. A zero-filled target therefore decodes to a perfectly correct,
entirely transparent picture, and what reaches the screen is the panel's backing
rectangle with nothing in it — which reads as a broken decoder and is not one.
The buffer is filled with `0xFF` once at open, so alpha is right and never
written again. `--comm-test` checks every pixel is opaque, because the failure
is silent and the fix is one byte per pixel that nothing else would notice.

## A transmission's voice

`Ambient::Comm` is a third channel beside the engine and the klaxon, and it is
the first one that carries a different recording each time it is used. A clip
directory may hold a `comm.wav` beside its pictures; `Bank::loadComm` scans
`assets/comm` and appends what it finds to the ordinary bank. A clip without one
plays silently, which is not an error.

It is on a channel rather than being a one-shot for the protection: `claimSlot`
never steals a channel, and a 20-second voice is otherwise the ideal victim the
moment a firefight fills the 32-voice pool. It does not loop, which is what
`Command::loop` was separated from channel ownership for.

### Two findings in taking a channel twice

Reusing a channel for a different recording was broken in two ways that only a
channel like this could reach, because the engine and the klaxon each play one
file forever.

`SetAmbient` updated the running voice in place — gain, pitch, position — and
never reassigned `clip`. A second transmission was therefore swallowed whole:
the channel kept playing the first to its end and the new one was never heard.
Comparing the clip and restarting when it differs is the fix.

Restarting alone then leaked. `start()` claims a *free* slot, so the voice
holding the old recording kept playing in a slot the channel no longer pointed
at — still flagged as a channel, so never stolen, and with nothing able to stop
it. The old voice is now retired before the channel is taken again.

### Sync is start-together and nothing else

The picture runs on the application clock and the voice runs on the audio
device's, and nothing tries to hold them in step beyond starting them in the
same frame. Over a transmission's length the two clocks drift by well under the
threshold either is worth correcting for, and the larger offset is the device's
own output latency at the start, which is a few milliseconds.

The duration a transmission is held for is declared in more places than agree
with each other, and a voice can outlive its own panel; both are written up in
`docs/KNOWN_ISSUES.md` rather than fixed here.

Doing better would mean the mixer publishing a playback cursor and the video
slaving to it, which is the standard answer and is not built. That is a
deliberate stopping point, not an oversight: it buys nothing until something
needs close lip-sync, and `--comm` plays no voice at all so screenshots stay
reproducible either way.

Nothing pauses, for the reason the comm player already did not: the mixer has no
pause and no way to address a voice once started, so pausing the picture alone
would leave a voice talking over a frozen frame.


## Transmissions are level content

Entering a level shows nothing. Nothing raises a transmission on its own: the
only thing that starts one is a level script calling `comm("<message id>")`, and
the message names the clip. A level with no `.lua` beside it has no
transmissions, and a level that wants one says when.

Before this, a level opened by playing whatever `kDefaultCommClip` named, and
`mine01` then showed the same `intro` clip twice more from its script. The
opening showing was not authored by anything — it was a default in `main.cpp`,
so every level got it whether it suited the level or not, it re-fired on every
restart, and a script trigger placed near the spawn would have cut it off
mid-play. Deleting it leaves one way in, which is the script.

`--comm <name>` still pins a clip to a fixed frame with no voice, and is now the
only thing that puts a transmission on screen without a script. That is what
keeps a panel screenshot reproducible.

`startComm()` still runs when a level starts, and now only clears: it stops the
player and the voice channel. That is what retires a transmission the previous
attempt was part way through when a level is restarted.
