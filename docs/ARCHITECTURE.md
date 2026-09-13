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

Status: none are implemented yet. M0 only puts the preconditions in place —
see "Reversed-Z" and "Render target" below, both of which exist to serve bet 3.

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

## Frame structure (M0)

```
bind MSAA target
  clear colour + depth + stencil
  scene pass            (textured, one draw)
resolve (blit) to the default framebuffer
ImGui on the default framebuffer
swap
```

ImGui is drawn *after* the resolve, not into the MSAA target. Its geometry is
axis-aligned text and rectangles that gain nothing from multisampling, and
keeping it out means the resolve carries only scene colour.

M2 inserts the depth prepass and the per-light passes between the clear and the
resolve. The shape it grows into:

```
depth prepass                          (all visible geometry, depth only)
for each visible light:
    clear stencil
    shadow volume pass                 (no colour write, depth-fail counting)
    additive lighting pass             (blend ONE,ONE; depth EQUAL; stencil==0)
unlit pass                             (emissive, bolts, HUD, automap)
resolve -> ImGui -> swap
```

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
