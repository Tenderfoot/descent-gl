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
identically to `depth.vert` including a `uModel` that is always identity. With
a different association the coincident cap lands a bit off, scattered pixels
count as behind, and the walls crawl with stripes of false shadow.

### Why depth-fail and not depth-pass

Depth-pass is cheaper and breaks the moment the camera is inside a shadow
volume — which, in a mine lit by a headlight, is most of the time. Counting what
is behind the visible surface instead of what is in front of it makes the near
plane irrelevant.

### The segment-set boundary rule

For the mine, only wall triangles in the light's segment set are considered, and
an edge with just one triangle inside that subset is treated as **not** a
silhouette. That leaves the volume open at the subset's border.

It is safe because the border portals are all farther from the light than its
radius: the opening is out beyond the distance at which this light contributes
exactly zero, and the lighting pass only ever draws segments in the same set.
Closing it would mean extruding walls the light cannot reach, every frame, to
affect pixels that are never shaded.

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

### `--collision-test`, and two wrong oracles

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

The collision code was correct throughout. **If a test disagrees with the code,
the test is a suspect too** — and here it was the culprit twice running.

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

At most `shadowBudget` lights (default 8) **cast** per frame. Every light still
*lights*; the budget only decides which also cast, because casting is what
costs: a silhouette extraction and a stencil pass per light per frame. Under
sustained vulcan fire there can be thirty bolts in the air, and thirty stencil
passes is not a frame.

Priority is deliberate:

1. The level's own lights, but only those the portal walk says are visible — an
   off-screen light casting into nothing is the cheapest thing to give up.
2. The headlight.
3. Everything dynamic, **nearest to the camera first**. A bolt across the room
   contributes a shadow nobody can resolve.

The overlay reports slots used and how many lights lit without casting, so the
cap can be watched rather than assumed.

### Resources

The headlight drains energy and **switches itself off at zero**. In a game with
no ambient term that is a real consequence, which is the point of it costing
anything. Shields at zero resets to the player start.

### Fire points

`kShipFirePoints` in `src/sim/ship.h` must stay on the ship model's widest
vertices — `assets/models/ship.obj` puts them at x = ±2.0 — or bolts appear out
of thin air beside the hull. Laser and vulcan alternate between them.

### Effects

Impact flashes and explosions are short-lived additive billboards in the unlit
pass, expanded on the CPU into one buffer and drawn in one call. There is no
particle system beyond that, deliberately.

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
