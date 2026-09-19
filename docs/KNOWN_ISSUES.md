# Known issues

Things that are understood, reproducible, and deliberately not fixed yet. Each
entry says what is wrong, where, what it costs, and what to do instead in the
meantime. Something that is merely unbuilt belongs in `MILESTONES.md` under the
milestone that did not build it; this file is for behaviour that is already
shipped and already wrong.

---

## The panel's display duration still has more than one source of truth

**Where:** `assets/comm/<clip>/comm.json`, `assets/levels/*.lua`,
`src/render/comm_clip.cpp`.

**Partly fixed.** `tools/make-comm-video.py` no longer writes `framesPerSecond`
or `displaySeconds` for a decoded clip — the stream carries both and the loader
reads them from it — and `testcard`'s manifest has been cleaned, so the
committed disagreement (a manifest claiming 20.0s for a 19.833s video) is gone
and the tool can no longer create another. The tool also clamps `--seconds` to
the source's real length and writes the voice track at that same length, so a
clip it produces has its video and wav agree by construction.

What remains:

- A manifest *may* still declare `displaySeconds`, and should when the intent is
  to hold a clip on screen for longer than it runs. That is the field doing its
  job, but nothing distinguishes it from a stale copy of the stream's length.
- For a frame sequence the field is required and means something different —
  `intro` is 16 frames at 8 fps, 2.0s of pictures held for 4.5s by looping.
- A Lua message's `seconds` overrides whatever the clip says. This is ordinary
  layering rather than duplication: a clip cannot know that one use of it wants
  a short beat.
- Nothing cross-checks any of it. `--comm-test` does not compare a declared
  duration against the stream, or the voice track against either.

**To fix:** have `--comm-test` check the declared durations against the stream
and the wav, so an authoring mismatch fails headlessly instead of being noticed
by ear.

**Note:** `docs/ARCHITECTURE.md` claims, in the section on decoupling the comm
player from the simulation, that collapsing `GameConfig::commSeconds` made "the
three-clocks-must-agree problem stop existing". That was true when written and
is not true now — the Lua override arrived with the multiclip work and the voice
track arrived after it.

---

## A transmission's voice outlives its panel

**Where:** `src/main.cpp` — `stopComm()` is called only for the `--comm`
screenshot pin and on a level switch.

A message's `seconds` governs the picture and nothing else. Nothing stops the
voice when the panel retires, so a clip whose `comm.wav` is longer than the
message keeps talking with no panel on screen. With `testcard` on `lower_mine`
that would be 14.5 seconds of audio after the panel closed.

**Workaround:** author `seconds` to match the voice track's length. A clip built
by `make-comm-video.py` now has its video and wav the same length already, so
matching `seconds` to the video is enough.

**To fix:** make `seconds` authoritative for both, which is really the same
decision as the entry above — this is what "nobody owns the duration" looks like
in the mixer.

---

## Nothing triggers a decoded clip at runtime

**Where:** `assets/levels/mine01.lua`, `src/main.cpp` (`kDefaultCommClip`).

`testcard` is the only decoded clip and nothing reaches it. Both of `mine01`'s
Lua messages name `intro`, and nothing else raises a transmission at all now
that a level no longer opens with one. So in a running game:

- sequential decode never happens — `--comm testcard` pins one frame, so
  `CommVideo::frame` returns pixels once and null on every later draw;
- `TextureArray::updateLayer` runs exactly once per run, so the
  `glTextureSubImage3D` + `glGenerateTextureMipmap` cadence at 24 fps is never
  exercised;
- `Bank::loadComm` loads `testcard/comm.wav` and `Audio::playComm` is never
  reached with a real clip, because `intro` has no voice track. The committed
  wav is never heard by anything.

`--comm-test` covers the decoder hard (forward, seek, wrap, opacity,
allocations) but is headless by design and touches no GL.

**Workaround:** point a `mine01.lua` message's `clip` at `testcard` and run
`--segment 12`, which fires the trigger on the first simulation step. Revert
before release: it is shipped level content. This is now the only way to see a
transmission play, since `--comm` pins a single frame and nothing opens a level
with one.

**To fix:** either a flag that plays a named clip instead of pinning it, or a
test level that is not shipped content.

---

## The comm panel is described as much smaller than it is

**Where:** `src/render/comm_video.h`, `third_party/pl_mpeg/README.md`.

Both say the pictures land in "a panel about ninety pixels wide". They do not.
`panelW = min(w * 0.28, 92 * s)` where `s = max(1.5, height / 260)`, so 92 is a
base before a scale that is never below 1.5. Measured on a 1080p frame: 395 px
including the border, 382 px of image.

| Window | Panel image |
| --- | --- |
| 1280x720 | 255 px |
| 1920x1080 | 382 px |
| 2560x1440 | 510 px |
| 3840x2160 | 764 px |

This matters because it is the number anyone would use to pick a source
resolution, and it understates it by a factor of four at 1080p. A 256x192 source
is already being upscaled 1.49x at 1080p; 384x288 is very nearly pixel-native
there.

**To fix:** correct both comments. No code change.

---

## The draw path allocates

**Where:** pre-existing, not introduced by any recent milestone.

Every run reports `draw path SOME on 29 of 30 (worst 6)` after warm-up.
CLAUDE.md says both allocation lines should read `none` and that a number there
is a finding rather than noise. The sim-step line is clean; this one is not, and
has been identically non-zero on `main` across every branch measured against it.

Untraced. It has never been attributed to a specific call site.

---

## A scaled object's collision stops at its own segment's walls

**Where:** `sim::separateFromObjects` in `src/sim/collision.cpp`, and the
`obj.segment = findSegment(level, obj.position, ...)` at the end of
`src/game/level.cpp`.

Object collision is gated to one segment: a body only ever tests the objects
whose `segment` matches its own. That is deliberate and the comment says why —
an author puts a column in the middle of a room, and widening the test to
neighbours would let a pillar standing against a wall shove a body in the room
on the other side of it.

Scaling breaks the assumption it rests on. An object's segment comes from
`findSegment` on its **origin point** and never sees its extents, so an entity
scaled up in the editor until it spills through a portal, the floor or the
ceiling is drawn on the far side, casts stencil shadows on the far side, and has
no collision there at all. `mine01`'s rooms are about 22 x 29.5 x 22 and a
pillar is 4.4 x 18, so it takes roughly 2x vertically, or less than that placed
near a wall, to reach it.

This is independent of the collider's shape. The convex-hull kind made a
column's collision identical to its model *within* a segment; it does nothing
for the part of a large object that has left one.

**What it costs:** a body can fly through the overhanging part of a heavily
scaled object. Nothing in a shipped level is scaled at all, so no shipped level
reaches it.

**In the meantime:** keep a scaled object inside its own segment. The load-time
proxy line reports each type's dimensions, and `--collision-test` reports the
standoff against the column's own surface, but neither warns about spill.

**To fix:** test the neighbours of a body's segment when the object's
`boundingRadius()` actually reaches past its own segment's bounds, rather than
widening the rule for every object; and assign an object's segment from its
extents rather than from its origin. Both are level-representation changes, not
proxy ones.

---

## The editor draws no collision proxy

**Where:** `tools/editor/src/view.js`.

The editor draws each object as its true mesh scaled by `object.scale`, which is
exactly what the game draws. It does not draw the proxy the simulation will
build from that model, so an author placing or scaling an entity has no way to
see what it will actually collide as.

**What it costs:** a mismatch between collision and model is invisible until
someone flies into it. The bounding-box-on-a-column case shipped for several
milestones for this reason — at scale 1 the gap hid inside the ship's own
radius, and only scaling an entity made it obvious.

**To fix:** draw the proxy as a wireframe on the selected object, derived the
same way `gl::modelTaperedCylinder` derives it, so the two cannot drift.

---

## A model that is not convex still collides as its bounding box

**Where:** `gl::modelConvexHull` in `src/render/model.cpp`.

A convex model gets a collider that is exactly the mesh the renderer draws. A
model that is not convex is rejected and keeps its bounding box, because the
only shapes available to it are a convex hull — which fills in its concavities
and is therefore not the shape on screen either — or per-triangle collision,
which the project does not do in the sim step.

Of the shipped models, `pillar.obj` and `drone.obj` are convex. The others are
not, by a corner standing this far outside one of their own faces:

| Model | Worst corner | Hull volume | Box volume |
| --- | --- | --- | --- |
| `brute.obj` | 1.00u | 1.06x the mesh | 1.80x |
| `gunner.obj` | 3.07u | 1.49x | 2.97x |
| `reactor.obj` | 7.20u | 1.76x | 3.70x |

**What it costs:** a shot or a ship stops against the reactor's bounding box,
which holds 2.7 times the volume the reactor does. That is the pre-existing
behaviour and nothing about it got worse, but it is now the largest remaining
divergence between collision and model in the game.

**In the meantime:** the load-time proxy line names every model that fell back
and says how far from convex it was, so this is visible rather than assumed.

**To fix:** a real convex hull would roughly halve the error for all three
without any new query code — the hull is the minimal convex set containing the
mesh, so it is never worse than the box — at the cost of a hull algorithm and
the judgement call that a filled-in concavity is acceptable. Exactness needs
convex decomposition, which is several hulls per model and a broad phase between
them.
