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

Status: bets 1 and 2 landed in M2 — see "The level format" and "The lighting
frame" below. Bet 3 is M3's; M0 put its preconditions in place, see "Reversed-Z"
and "Render target".

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
      [M3: clear stencil, shadow volume pass]
      additive lighting pass           (blend ONE,ONE; depth EQUAL; this
                                        light's segments only)
  unlit pass                           (emissive and debug; M5 bolts and
                                        flares, M7 HUD, M8 automap)
resolve (blit) to the default framebuffer
ImGui on the default framebuffer
swap
```

ImGui is drawn *after* the resolve, not into the MSAA target. Its geometry is
axis-aligned text and rectangles that gain nothing from multisampling, and
keeping it out means the resolve carries only scene colour.

The per-light loop in `Renderer::lightPasses` carries a marked insertion point
where M3 adds the stencil clear and the shadow volume pass. It is shaped that
way so that milestone inserts rather than restructures.

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
