# Milestones

What is done, and what was actually verified for each. Numbers are measured on
the development machine (RTX 4070 Ti, i9-14900K, Windows 11, NVIDIA 591.86)
unless noted.

---

## M0 — Skeleton ✅

Plumbing only, no gameplay. Window, GL 4.6 core context, MSAA depth-stencil
target, reversed-Z, textured cube, ImGui, fixed timestep, docs.

### Deviation from the plan

The plan specified Vulkan 1.3. This was changed to **OpenGL 4.6 core** before
implementation, on the grounds that Vulkan's advantages do not apply at this
project's scale while its plumbing cost is permanent, and that stencil shadow
volumes are native to GL. Full reasoning in `docs/ARCHITECTURE.md`. The
consequence for M0 was a smaller dependency set (`glad` in place of
volk / vk-bootstrap / VMA), no Vulkan SDK prerequisite, and no offline shader
compile step. Every later milestone is unaffected in substance: they describe
passes and state, not API objects.

### Verified

| Check | Result |
| --- | --- |
| Debug builds clean | ✅ MSVC 14.51, `/W4 /permissive-`, no warnings |
| Release builds clean | ✅ |
| Configure from clean tree | ✅ vcpkg installs 8 packages, 27 s warm cache |
| GL context | ✅ `GL 4.6.0 NVIDIA 591.86`, RTX 4070 Ti |
| Render target | ✅ 4x MSAA, `DEPTH32F_STENCIL8`, stencil aspect present |
| Reversed-Z | ✅ `glClipControl` ZERO_TO_ONE, `GEQUAL`, clear 0.0; cube faces resolve correctly, no z-fighting |
| Textured cube | ✅ see `build/Debug/m0-debug.png` |
| Backface culling | ✅ CCW front, back faces culled |
| ImGui overlay | ✅ frame time, sim steps, interp alpha, framebuffer size, placeholder panel for M1 |
| KHR_debug fatal-on-high | ✅ installed in Debug; no error-severity messages in any run |
| Resize | ✅ 1600x900 → 1024x768 → 800x600 → 1920x1080 → 640x360, target rebuilt each time |
| Iconify / restore | ✅ 64–72 frames correctly skipped while minimized, no rebuild, no crash, clean restore |

### Fixed timestep

The check that it is real rather than cosmetic is the achieved step rate
against the 120 Hz target, across wildly different frame rates:

| Run | Frames | Frame rate | Steps | Rate |
| --- | --- | --- | --- | --- |
| Debug, steady state | 20000 | ~7000 fps | 341 | **119.7 / 120** |
| Debug, resize test | 1799 | 200 fps (vsync) | 900 | **119.8 / 120** |
| Release, steady state | 40000 | ~9600 fps | 500 | **119.8 / 120** |
| Release, resize test | 1799 | 200 fps (vsync) | 900 | **119.7 / 120** |

The residual 0.2% is the leftover accumulator fraction at exit, as expected.

Two measurement bugs were found and fixed while establishing this, both now
documented in `docs/ARCHITECTURE.md`:

- The final screenshot's PNG encode (~200 ms) was inside the measurement
  window, making the rate read 113.6 instead of 119.8. The clock now stops
  before the capture.
- Time spent minimized is discarded by the sim by design, but was still in the
  denominator, making `--resize-test` read 100.0. It is now excluded and
  reported separately.

### Known-good screenshots

- `build/Debug/m0-debug.png` — rotated textured cube, ImGui panel
- `build/Release/m0-release.png` — same, Release

### Not done in M0, by design

No gameplay, no level format, no lighting model, no shadow volumes. The stencil
aspect exists and is multisampled but is untouched. `src/sim` and `src/game`
hold only the fixed-timestep state.

---

## M1 — 6DOF flight ✅

Quaternion 6DOF ship, tunable flight model, chase camera, hand-authored ship
mesh, reference environment. No levels, no collision, no weapons.

### Verified

Measured in the Linux dev container (zink over lavapipe, software GL — see the
note at the end of this entry).

| Check | Result |
| --- | --- |
| Debug builds clean | ✅ `-Wall -Wextra -Wpedantic`, no warnings |
| Release builds clean | ✅ |
| Ship mesh loads | ✅ `assets/models/ship.obj`, 13 vertices, 22 triangles |
| Ship mesh is closed | ✅ 33 edges, every one shared by exactly 2 triangles; Euler 13−33+22 = 2 |
| Ship winding | ✅ signed volume +3.724, i.e. outward |
| Chase camera | ✅ F1, and `--chase` to start in it |
| Flight config | ✅ loaded from `assets/config/flight.json`, live sliders, Save / Reset / Reload |
| No gimbal lock | ✅ orientation is a quaternion throughout; no Euler angle is stored anywhere |
| GL error-severity messages | ✅ 0 |
| `--resize-test` | ✅ 5 sizes, target rebuilt each time, 119.9 / 120 |

### Flight model, measured

`--flight-test` runs the real `stepShip` at the real 120 Hz with no window and
no GL, and fails the process if any number is out of range:

| Measurement | Result | Wanted |
| --- | --- | --- |
| Top speed | 19.00 units/s (terminal 20.00, cap 20.0) | — |
| Spin-up to 95% of top speed | **1.09 s** | 0.7 – 1.4 |
| Coast to 5% of top speed | **0.98 s** | 0.7 – 1.4 |
| 800 px mouse swipe | **174.3°** | 150 – 210 |

Identical in Debug and Release, as it must be: the sim has no configuration
dependence.

### Step rate

| Run | Frames | Steps | Rate |
| --- | --- | --- | --- |
| Debug, steady state | 90 | 491 | **119.9 / 120** |
| Debug, resize test | 573 | 1080 | **119.9 / 120** |
| Release, steady state | 120 | 641 | **119.8 / 120** |
| Release, resize test | 562 | 1080 | **119.9 / 120** |

### The constants, and why

| Constant | Value | Reasoning |
| --- | --- | --- |
| `linearDamping` | 3.0 /s | tau = 0.33 s. Three time constants is ~1 s, which is both the spin-up and the settle target, so one number buys both. |
| `forwardThrust` | 60.0 | Terminal speed under exponential drag is thrust/damping, so this is `maxSpeed * linearDamping` exactly. |
| `strafeThrust`, `liftThrust` | 42.0 | 70% of forward. Sliding is useful but should never be the fast way to travel. |
| `maxSpeed` | 20.0 units/s | One 20-unit segment per second, the scale M2's mine is authored to. |
| `angularDamping` | 8.0 /s | tau = 0.125 s. Turns settle fast enough to aim with, slow enough to still read as a ship with mass. |
| `mouseSensitivity` | 0.0315 (rad/s)/px | A swipe rotates by `impulse / angularDamping`, so this is π·8/800 — 180° per 800 px. |
| `rollTorque` | 20.0 rad/s² | Steady roll rate of 2.5 rad/s (143°/s), under the 4.0 cap. |
| `pitchTorque`, `yawTorque` | 14.0 rad/s² | Keyboard turning is deliberately slower than the mouse; it is a fallback, not the primary aim. |
| `maxAngularSpeed` | 4.0 rad/s | Ceiling on thruster rotation only. |

### What the flight test caught

A full mouse swipe turned the ship **29°, not 180°**, and then 39° after a
first attempt at a fix. The cause was `maxAngularSpeed` clamping the combined
angular velocity: the mouse impulse survived the step it arrived on, and the
clamp caught what remained of it on the *next* step and ate the rest of the
turn. Both the bug and the incomplete fix were invisible by eye and obvious as
a number.

The fix is two angular channels — `thrusterSpin`, capped, and `aimSpin`,
uncapped — summed to produce the real angular velocity. The alternative was
raising the cap until it stopped biting, which would have made it useless for
the thing it exists for. Documented in `docs/ARCHITECTURE.md`.

### Deviation from the plan

The M1 prompt asks for mouse pitch/yaw with "a max angular speed" over one
angular velocity. That combination cannot hold both a meaningful thruster cap
and a correct swipe-to-turn ratio, so the cap was narrowed to thruster-driven
rotation. Everything else in the prompt is as written.

### Not verified here

Two things this container cannot check, both for the dev machine:

- **Feel.** 1.09 s and 174° are the numbers the targets ask for, but whether
  the ship is *fun* is a judgement only playing it can make. The sliders and
  Save exist for exactly that.
- **Iconify / restore.** `--resize-test` reports 0 frames skipped while
  minimized because Xvfb runs with no window manager, so `glfwIconifyWindow`
  is a no-op. The resize half of the test is genuinely exercised; the
  iconify half is not. It passed on Windows in M0 and that path is unchanged.

Software rendering also means the frame rates here (11–77 fps) say nothing
about performance. M8 takes those numbers on the real machine.

---

## M2 — Segments, portals, lights ✅

The segment-graph level format, portal-traversal visibility, and the multi-pass
lighting frame. Two of the three permanent bets land here. No shadows and no
collision: the ship flies through walls this milestone, as planned.

### Verified

| Check | Result |
| --- | --- |
| Debug / Release build clean | ✅ no warnings |
| `test01` loads and validates | ✅ 14 segments, 14 portals, 112 wall triangles, 8 lights, 58 vertices |
| Portal reciprocity + shared vertices | ✅ enforced at load, fatal on failure |
| Non-planar side split | ✅ **42 of 84 sides take the alternate diagonal** at the minimum-crease rule |
| Texture array | ✅ 256×256 × 5 layers (rock, panel, hazard, grate, hull) |
| Unlit surfaces render black | ✅ no ambient term exists anywhere |
| Falloff reaches zero at the radius | ✅ by construction, and checked numerically per segment |
| Light editor Save | ✅ save/reload round trip preserves 14 segments, 8 lights, 112 walls |
| Freeze-visible-set toggle | ✅ |
| GL error-severity messages | ✅ 0 |
| `--resize-test` | ✅ 119.7 / 120 |
| `--flight-test` | ✅ still PASS, unchanged by this milestone |

### Drawn-segment counts

`--world-test` runs the portal walk from every segment over all six axis
facings, with no window and no GL. Three positions the plan asks about:

| Position | Visible best | Visible worst | Lights reaching the view |
| --- | --- | --- | --- |
| Segment 0, start of the corridor | 4 | 1 | 4 |
| Segment 7, the 2×2 room | 8 | 3 | 5 |
| Segment 11, foot of the shaft | 10 | 3 | 8 |

At most **10 of 14** segments are visible from anywhere in the mine, so the
cull is doing real work rather than passing everything through.

### Lit pools and dark stretches

Brightness at each segment centre, computed with the same falloff the shader
uses:

| Segment | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Brightness | 1.80 | **0.09** | 1.62 | 1.18 | **0.12** | 2.09 | 0.66 | 3.03 | 0.71 | 0.65 | 2.83 | 2.19 | 0.49 | 3.39 |

Segments 1 and 4 are below the dark threshold — genuinely black stretches
between the lit pools, which is the point of the no-ambient bet. The world test
fails if no segment is dark, because a mine with nowhere to hide is a level bug
that does not crash.

### Two bugs worth recording

**Shader invariance.** Walls rendered shredded with thin unlit lines.
`wall.vert` computed `uProj * uView * (uModel * p)` while `depth.vert`
computed `uProj * uView * uModel * p` — the same value in exact arithmetic,
different in the last bit of a float, so the lighting pass's `GL_EQUAL` test
against the prepass depth rejected scattered fragments. Fixed by declaring
`invariant gl_Position` in every pass that draws into this depth and spelling
the expression identically. Nothing about the symptom pointed at the vertex
shader.

**Near-plane portal clipping.** The first portal walk reported *all 14
segments visible from anywhere in the mine*, and `--world-test` failed on
"portal culling is doing nothing". A portal beside the camera straddles the
near plane; projecting a corner behind the eye divides by a negative `w` and
mirrors it across the screen. The first version detected that and fell back to
the parent window, which is conservative and useless — at a segment's centre
nearly every portal straddles. Fixed with a Sutherland-Hodgman clip against
`w > epsilon` before projecting.

Both were invisible as code and obvious as output, which is what the headless
tests are for.

### Light falloff scale

An absolute inverse-square falloff (`1/(d² + 1)`) left the mine essentially
black at 20-unit segment scale: it wants intensities in the hundreds and still
gives a pinprick at the source. Changed to radius-relative, so `radius` sets
both reach and shape and `intensity` stays a small readable number. Level
intensities are 2.2–3.4.

### Removed

The M1 reference environment went with the milestone that needed it:
`makeInsideBox`, `makeGridTile`, `shaders/textured.*`, `assets/textures/` and
`Texture2D` (nothing loads a texture from a file any more — every tile is
generated). `stb_image`'s loader stays compiled in `stb_impl.cpp` for the
screenshot writer's sake.

### Not verified here

Iconify/restore, as in M1 — Xvfb has no window manager. Software rendering
makes the frame rates (11–14 fps) meaningless as performance; M8 takes those
numbers on the real machine.

---

## M3 — Stencil shadow volumes ✅

The visual identity. Every light now casts pixel-exact hard shadows from every
mesh and from the mine walls themselves, plus a shadow-casting headlight.

### Verified

| Check | Result |
| --- | --- |
| Debug / Release build clean | ✅ no warnings |
| Every mesh closed | ✅ ship 13v/22t/33e, drone 12v/20t/30e, pillar 16v/28t/42e |
| **Mine walls are one closed surface** | ✅ V 58 − E 168 + F 112 = **2** (genus 0) |
| Silhouette extraction live | ✅ 30 silhouette edges across the 8 level lights |
| Hard shadow from a drone between a light and a wall | ✅ |
| Headlight sweeps shadows as the ship turns | ✅ 34° cone, shadow casting, H toggles |
| Camera inside a shadow volume | ✅ depth-fail; all 14 segments flown, nothing breaks |
| Every segment, no GL errors | ✅ 14/14 clean in Debug (fatal-on-high) |
| Volume wireframe view | ✅ `--wireframe-volumes` |
| Stencil visualiser | ✅ `--stencil-view` |
| Per-light shadow disable | ✅ checkbox per light in the shadows panel |
| `--world-test`, `--flight-test` | ✅ PASS in both configs |
| `--resize-test` | ✅ completes, both configs |

### Silhouette edges per light, on the mine walls

From `--world-test`, computed on the CPU with no GL:

| Light | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Silhouette edges | 0 | 0 | 6 | 6 | 9 | 3 | 6 | 0 |
| Segments in set | 2 | 3 | 5 | 4 | 8 | 7 | 6 | 3 |

Lights 0, 1 and 7 produce **zero** wall silhouettes, and that is correct rather
than broken: they sit in straight stretches whose interior is star-shaped about
them, so every wall faces the light and nothing occludes. The lights at bends
and in the room are the ones with silhouettes. A total of zero would mean the
extraction was broken, and `--world-test` fails on exactly that.

In the room with the headlight on: 25 casters, 206 silhouette edges, 1322
volume triangles per frame.

### Three bugs, in the order they appeared

**1. Crawling stripes of false shadow on every wall.** The volume's front cap is
the occluder's own light-facing triangles, exactly coincident with the geometry
the depth prepass drew. `shadow.vert` computed its position by a different
association than `depth.vert`, so the last bit differed and scattered pixels
counted as "behind". Fixed with `invariant gl_Position` and an always-identity
`uModel` so the expression is character-for-character the same. (The same class
of bug as M2's shredded walls — a third pass that draws into this depth, a
third time it mattered.)

**2. The entire mine rendered black.** With the stripes gone, *every* lit
surface shadowed itself. Depth-fail counts faces strictly behind the surface, so
the coincident front cap must be counted to cancel the back cap at infinity. A
conventional `GL_LESS` buffer fails a coincident fragment and cancels
automatically; **reversed-Z with `GL_GEQUAL` makes equal pass**, so the cap
stopped counting. Fixed by using `glDepthFunc(GL_GREATER)` for the volume pass
only. This is a direct consequence of the M0 reversed-Z decision and is the kind
of thing that is invisible until the whole screen is black.

**3. The player shadowing themselves.** In first person the camera is inside the
ship's hull, so the ship's shadow volume began at the eye and blacked out most
of the view. The ship is now an occluder only in the chase camera, where it is
actually drawn, and never for its own headlight.

### Step rate, and the frame clamp

Software rendering at 1600×900 with shadow volumes runs at about 2 fps, which is
slower than the loop's 0.25 s frame clamp. The clamp then discards the excess,
so the reported step rate is bounded by the clamp rather than by the simulation:

| Run | Step rate |
| --- | --- |
| 1600×900, 8–25 frames | 46–93 / 120 (clamp-bound) |
| 640×360, 200 frames, Debug | **119.9 / 120** |
| 640×360, 400 frames, Release | **120.0 / 120** |
| 320×180, 300 frames | **119.9 / 120** |
| `--resize-test` (peaks at 1920×1080) | 95–96 / 120 (clamp-bound at the large sizes) |

`--size W H` was added for exactly this: at any size where a frame fits inside
the clamp, the rate sits on 120. On the dev machine's GPU none of this arises.

### Not verified here

- **Light leaks at segment boundaries.** All 14 segments were flown and rendered
  without artifacts, and the mine wall occluder validates as one closed surface,
  which is the structural precondition. Whether a leak is visible anywhere in
  practice is a judgement for the real machine.
- Iconify/restore, as before — Xvfb has no window manager.
- Performance. M8 takes those numbers on the 4070 Ti.

---

## M4 — Collision ⬜

Not started.
