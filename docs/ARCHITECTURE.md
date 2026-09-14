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
returns immediately in `Won` and `Lost`, so an end screen is a frozen
simulation rather than a separate mode with its own loop. The phase never runs
backwards; restarting rebuilds the state from the level instead of unwinding it.

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

A wall side is drawn as its outline. A portal to a **visited** segment is drawn
as nothing at all, so a corridor reads as a gap rather than as a line to bump
into. A portal to an **unvisited** one gets a faint outline: a frontier marker
that says there is a way on without saying what is through it.

`GameState` records the knowledge, and the rules are deliberately mean:

- **`visited`** is segments the ship has actually been inside — not segments it
  has *seen* down a corridor, which would hand the player the layout of a room
  they glanced into.
- **`seen`** marks a pickup whose segment has been visited, so a key noticed and
  left behind can be flown back to.
- **`reactorSeen`** needs a real line of sight, tested with the same `raycast`
  the robots use and against the doors as they stand that step, so a reactor
  behind a shut door is not revealed through it. It runs only until it succeeds
  once — a handful of rays per level, not per step.

None of it feeds back into the simulation. A level plays out identically whether
the map is ever opened or not, which is what makes it safe for `stepGame` to
write and for the renderer to read.

### Framing

The orbit camera frames the **visited** bounding box rather than the level's, so
the map stays legible early on instead of showing one lit room in the corner of
an empty box. It grows as the player explores.

`--reveal` draws every segment and every marker as though found. It is a
rendering check for the map itself — the same family as `--wireframe-volumes`
and `--stencil-view` — and it touches only what the renderer is handed, never
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
