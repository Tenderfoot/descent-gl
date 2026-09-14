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

## M4 — Collision ✅

Sphere-versus-segment collision with sliding, and the ray and sweep queries
every later milestone consumes. No gameplay change beyond the ship no longer
passing through walls.

### Verified

`--collision-test` runs the real resolve loop at the real fixed rate with no
window and no GL. Identical results in Debug and Release:

| Check | Result |
| --- | --- |
| **Containment** | **364 trials — 14 segments × 26 directions × 3 s of sustained full throttle at 20 u/s — 0 escapes** |
| Sliding | 56 walls entered at 45°, **0 caught**; every one covered more than a quarter of free-flight distance in a second |
| Portals | 28 crossings, every portal from both sides, **0 failed** |
| Raycast | 364 rays, 0 missed, **0 disagreed** with marching; worst error 0.050 units, which is the marching step size |
| Swept sphere | 364 sweeps, all 364 stopped short of the ray as they must, **0 passed through a wall the ray hit** |
| Debug views | wall normals, contact points and the nose raycast all draw (`--debug-lines`) |
| Debug / Release build clean | ✅ no warnings |
| `--world-test`, `--flight-test` | ✅ PASS in both configs |
| `--resize-test` | ✅ completes, both configs |
| Step rate, 640×360, Release, 300 frames | **120.0 / 120** |

The 26 directions are the face, edge and corner directions of a cube, so
"straight at a wall", "straight at an edge" and "straight at a corner" are all
covered rather than sampled.

### The bug hunt that wasn't

The first run reported 5 escapes and 125 of 364 raycasts disagreeing with an
independent method. Both were the **test's** fault, not the collision's:

1. The oracle started as `findSegment`, which walks the segment graph using one
   averaged plane per side. That is approximate for a non-planar side, and every
   side in this format may be non-planar. Replacing it dropped the raycast
   disagreements from 125 to 2.
2. The replacement was "nearest wall triangle, which side of it" — wrong at a
   convex edge, where the surface folds towards the interior as it does at the
   inside of every bend. A point in the wedge beyond such an edge has a negative
   dot with *both* adjacent faces' normals and reads as outside. Nine false
   escapes, and a probe showed the culprit exactly: a point genuinely inside
   segment 3 whose nearest wall was segment 1's front face, 8.7 units away and
   facing the wrong way.
3. The third oracle is ray parity against the closed wall surface. Zero false
   anything, and the raycast's worst disagreement is now 0.050 units — exactly
   the marching increment, i.e. exact agreement.

The collision code never changed. Worth recording because the instinct on a
failing test is to go looking in the code under test.

### Not verified here

- **Feel.** "Slides without stutter" is measured as distance covered, which is
  not the same as it feeling smooth at 144 Hz on a real machine.
- Iconify/restore, and performance, as in every milestone so far.

---

## M5 — Weapons, flares, energy ✅

Four weapons, projectile lights with a shadow budget, flares, impact and
explosion effects, and the player's energy and shield pools.

### Verified

`--weapons-test` fires everything at walls and at targets with no window and no
GL. Identical in Debug and Release:

| Check | Result |
| --- | --- |
| All four weapons fire | ✅ laser 140 u/s, vulcan 220, concussion 55, homing 46 |
| Rate of fire respected | ✅ no second shot inside the cooldown |
| Energy and ammo spent | ✅ laser energy, vulcan/concussion/homing ammo |
| Bolts hit walls | ✅ gone after 0.31 s, impact flash spawned |
| **No tunnelling at 2000 u/s** | ✅ 16.7 units per step — most of a segment — still stopped at the wall |
| Targets take damage and die | ✅ 30 health / 8 per shot = 4 shots, destroyed after 0.63 s |
| Splash damage | ✅ a deliberate near miss took a target from 30 to 21.4 |
| Flares stick and burn | ✅ landed after 1.36 s, burned 30 s, then went out |
| Vulcan spam | ✅ 5 s of fire, pool never near its 128 capacity, 86 rounds spent |
| Headlight drain | ✅ 1.60 energy/s against 1.60 configured; switches off at zero |
| Death resets to start | ✅ segment 0, shields restored, death counted |
| Shadow budget holds | ✅ overlay reports 6 of 8 slots used, 4 lights lit without casting |
| Debug / Release build clean | ✅ no warnings |
| All four headless tests | ✅ world, collision, weapons, flight — PASS in both configs |
| `--resize-test` | ✅ completes |
| Step rate, 640×360 under vulcan fire | **120.0 / 120** |

### Seen on screen

- A vulcan bolt in flight lighting the corridor floor and wall around it while
  the pillars keep their hard shadows.
- A flare thrown into the unlit room, landing, and lighting it warmly with a
  hard pillar shadow across the floor — which is the mechanic the no-ambient
  bet exists for.

### Notes

- Projectiles move by **swept sphere**, which is what the 2000 u/s test above is
  really checking: at that speed a position-only test steps straight through a
  wall.
- Fixed-capacity pools held by value in `SimState`, because `SimState` is copied
  every step and a vector there would allocate in the simulation loop.
- Homing acquires **once, at launch**. Re-acquiring every step makes it feel
  like a guided camera rather than a missile.
- The headlight switching itself off at zero energy is deliberate: in a mine
  with no ambient light that is a real consequence, and it is the point of the
  headlight costing anything at all.

### New harness flags

`--weapons-test`, `--autofire W` (hold a trigger for screenshots) and `--no-ui`
(hide the ImGui panels so a screenshot shows the game).

### Not verified here

Whether the shadow budget's *default of 8* is the right number for a real GPU —
the cap is enforced and observable, but the value to set it to is a performance
question M8 answers on the dev machine. Feel, iconify/restore and performance,
as in every milestone so far.

---

## M6 — Robots ✅

Three robot types with a five-state machine, segment-graph pathing, emissive
eyes, death debris and a drop table.

### Verified

`--robots-test` drives the state machine with no window and no GL:

| Check | Result |
| --- | --- |
| Three types, closed meshes | ✅ drone 12v/20t, gunner 28v/48t, brute 40v/72t — all closed, all under 120 |
| A robot that cannot see the player idles | ✅ **7 of 8** idle two seconds after spawn |
| Being shot wakes it | ✅ a sleeping robot went **idle → chase**, awareness 0.84 |
| Paths through portals | ✅ started in segment 2, reached 9 segments, **8 portals crossed** |
| Shoots, and hits hurt | ✅ shields 100 → 92 in **0.51 s** at 14 units in the open |
| Retreats when hurt | ✅ **12 of 12** trials |
| Dies, with debris and drops | ✅ 12/12 died, **4 debris chunks**, **9 of 12** kills dropped something |
| Robots never leave the mine | ✅ 8 robots chasing for 15 s, **0 escapes** |
| Per-robot state readout | ✅ mode, previous mode, health, segment, portal distance, awareness |
| Robot shadows | ✅ 43 casters / 543 silhouette edges / 2934 volume triangles with the room occupied |
| Debug / Release build clean | ✅ no warnings |
| All five headless tests | ✅ world, collision, weapons, robots, flight — PASS in both configs |
| `--resize-test` | ✅ completes |
| Step rate, 640×360 with robots active | **120.0 / 120** |

### Two test bugs, no AI bugs

The first run failed twice, and both were the test:

1. "Being hit wakes a robot" picked the first drone — which could already see
   the player down the corridor and was awake before it was shot. Fixed by
   picking a robot that is demonstrably asleep.
2. "A gunner's fire hurts the player" parked the player 12 units from the robot
   along +Z, which put it **outside the mine**. The robot genuinely could not
   see it, so it never attacked. Fixed by placing both relative to the
   segment's own centre.

Third milestone running where the first failure was in the harness rather than
in the code under test.

### Design notes

- **One BFS serves every robot.** A distance field from the player's segment,
  rebuilt only when the player changes segment; each robot walks downhill.
- **Awareness, not line of sight**, drives the state machine — seeing sets it
  to 1, losing sight decays it over three seconds, and being hit sets it too.
- Robots are `Body`s, so containment is the M4 collision code and not a second
  implementation of it.
- Eyes are emissive billboards rather than mesh faces. A deviation from the
  plan's "emissive eye geometry"; a glow reads better at distance and the
  meshes carry no material split.

### A build bug worth recording

Assets were copied next to the executable by a `POST_BUILD` step, which only
fires when the executable **relinks**. Editing `test01.json` and rebuilding left
the old copy in place, so the game loaded 6 objects from a file that contained
10 — the source on disk was right and the running program disagreed. It is now
an always-run target.

### Not verified here

Feel — whether the robots are *fun* to fight, whether retreat reads as cunning
or as cowardice, and whether the brute is too spongy. Those want the real
machine. Iconify/restore and performance as ever.

---

## M7 — The loop 🟡 In progress

Pickups, keyed doors, the reactor, the countdown, the exit, win and lose
screens, an in-game HUD, and `mine01`. **Paused mid-milestone at the user's
request.** The one known failure recorded below turned out to be the test's own
oracle and is now fixed; see *Fixes after M7 — the raycast oracle* at the end of
this file.

### Done and verified

`--loop-test` plays the win and lose conditions through with no window and no
GL:

| Check | Result |
| --- | --- |
| Three keyed doors | ✅ each: locked held, **every wrong key held**, right key opened |
| A shut door blocks light | ✅ a probe light reaches **11 segments shut, 18 open** |
| Reactor | ✅ 600 health, destroyed → countdown 45.0 s, exit unlocked |
| Mine shake ramps | ✅ 0.000 at the start of the countdown, 0.445 thirty seconds in |
| Reaching the exit wins | ✅ |
| Countdown reaching zero loses | ✅ "the mine went up with you still in it" |
| Shields at zero loses | ✅ "shields gone" |
| Pickups | ✅ **16 of 16** collected on touch and each had an effect |
| `mine01` loads and validates | ✅ **34 segments**, 41 portals, 244 wall triangles, 14 lights, 18 objects, 3 doors, 16 pickups |
| `mine01` wall surface is closed | ✅ 124 vertices − 366 edges + 244 triangles = 2 |
| 16 robots including the reactor | ✅ |
| HUD | ✅ shields and energy bars, weapon and ammo, flares, key indicators, reticle, countdown, end screens |
| ImGui behind F3 | ✅ debug panels are off by default from this milestone |

`test01` is kept and still loads; `--level PATH` selects it.

### Known failure: one raycast in 884 on `mine01` — RESOLVED, it was the test

`--collision-test` reports **1 disagreement of 884 rays** on `mine01` (it passes
cleanly on `test01`). Diagnosed, not yet fixed:

A ray from segment 24's centre heading exactly (−0.707, 0, −0.707) — straight
down a grid diagonal — is reported as hitting segment 24's *back* wall at 14.675
units. Marching the same ray shows it actually squeaks past the corner with
**0.241 units of clearance** and continues to about 71 units. So the raycast is
stopping short: the ray should have exited through segment 24's portal into
segment 26, and the portal-exit test missed at that grazing angle.

An attempted fix — widening the barycentric bounds of the portal test so a ray
along the shared diagonal of a portal's two triangles still counts — made things
worse: 5 rays then leaked *out* of the mine entirely. Reverted.

The failure is conservative in gameplay terms (a ray stops earlier than it
should, never later), it is confined to exact grid diagonals, and the swept
sphere is unaffected: **884 of 884 sweeps behave correctly**. It still needs a
proper fix before M7 is called done.

**Everything above this line is wrong about where the bug was.** `raycast` was
correct; the marching oracle this section trusted was not. Both the segment-24
diagnosis here and the segment-16 one recorded under the rendering follow-up were
the oracle terminating itself on a legitimate grazing pass. The section is kept
as written because the reasoning it contains is instructive — it treated
agreement between two methods as evidence about the faster one, and never
questioned the slower one. See *Fixes after M7 — the raycast oracle* below.

### The rest of the level flow

Containment, sliding and portal crossing all pass on `mine01` at its larger
size: 884 containment trials with 0 escapes, 122 walls slid without catching,
82 portal crossings.

### Doors

A door is a **portal in the graph** carrying a slab that fills the opening while
it is shut, rather than a new kind of side. The two segments still share their
four vertices, so the mine's wall surface stays closed and M3's occluder still
validates. One `DoorField` — a byte per segment side — is consulted by the
portal walk, the light flood fill and collision alike, so those three cannot
disagree about whether a door is shut.

### Three harness assumptions that broke on the bigger level

Worth recording, because all three were the tests and not the code:

1. The weapons test counted *all* live projectiles to decide whether the
   player's bolt had landed. On a level with robots near the spawn, robot fire
   is in the same pool and the count is never zero.
2. The robots test looked for "a robot in segment ≤ 2", which baked `test01`'s
   numbering in. It now picks whichever live robot is physically furthest from
   the player.
3. The collision test's marching oracle stopped only when the point left the
   mine, so a ray along a grid diagonal could march through the single point
   where two segments meet at a corner. It now also requires a little clearance
   from the surface.

### Also verified on this branch

| Check | Result |
| --- | --- |
| Debug / Release build clean | ✅ no warnings |
| World, weapons, robots, loop, flight tests on `mine01` | ✅ PASS |
| Same tests on `test01` | ✅ PASS (loop test reports SKIPPED: `test01` has no doors or exit) |
| `--resize-test` | ✅ completes |
| Step rate, 640×360, Release, 250 frames | **119.9 / 120** |

### Not done yet

- ~~**The raycast diagonal case above**~~ — fixed; it was the test's oracle, not
  the collision code.
- ~~`ARCHITECTURE.md` sections for doors, the HUD and the game flow.~~ — done;
  doors landed with the door fixes, the game loop and the HUD after that.
- Playtesting `mine01` end to end, which needs the real machine: whether the
  key routing is legible, whether 45 seconds is the right countdown, and
  whether 16 robots is too many or too few.

---

## Fixes after M7 — doors, phantoms, homing

Three gameplay bugs reported from play on the dev machine, all in `src/sim`:

- **Shots stopped mid-air for no reason.** Every level object was a hit
  target, including the 16 that had become robots, so each robot's spawn
  point was an invisible, indestructible 2.4-unit sphere. `initObjects` now
  marks robot entries dead. The renderer's prop list indexed object health
  by prop index rather than object index, which the fix exposed; corrected.
- **Robots moved and shot through doors, and bolts passed through them.**
  `SolidDoors` was built each step and handed only to the ship's collision.
  It now goes to `stepCombat` and `stepRobots` too: projectile sweeps, robot
  sight lines, robot containment and debris all stop at a shut door, and the
  path search treats a locked shut door as a wall.
- **Homing missiles never sought robots.** Acquisition only looked at level
  objects with health, which on `mine01` is nothing. It now prefers robots.

### Verified (Linux container, software GL; sim is configuration-independent)

| Check | Result |
| --- | --- |
| A bolt through a removed robot's spawn point | ✅ flew **15.3 units past** it on `mine01`, 49.0 on `test01` |
| Homing locks and hits | ✅ locked robot 0, health 26 → 0 |
| A bolt at a shut door | ✅ stopped **0.0 units** short of the door plane; the same shot carried through once the door opened |
| A robot chasing through a locked door for 5 s | ✅ **0 steps** on the wrong side, door stayed at 0.00, shields lost 0.0 |
| A robot behind a shut door | ✅ unseen for 1 s shut, seen within 3 s of opening |
| Weapons, robots, loop, world tests on `mine01` | ✅ PASS |
| Weapons, robots tests on `test01` | ✅ PASS (loop SKIPPED, no doors) |
| Pillars still drawn after the prop index fix | ✅ headless screenshot from segment 12 |
| `--collision-test` | ❌ still the pre-existing one-ray diagonal case, unchanged by this |

---

## Fixes after M7 — lights and doors

Reported from play: the room behind a door only appeared once the door was
fully open; one side of a doorway fizzed with shadow artifacts while the other
was clean; and a shadow budget of 32 always looked better than the default 8.

- **Doors are see-through as soon as they crack open.** `DoorField` now
  carries `blocked` (a body, below 0.75 open) and `opaque` (sight and light,
  fully shut only), and the portal walk and light flood consult `opaque`.
- **Level lights and the headlight respect doors.** Level light sets were
  computed once at load with no door state and never rebuilt; the headlight's
  per-frame rebuild also omitted the field. Only bolt lights used it. They are
  now rebuilt against the field at start and whenever a door's opaque state
  changes. On `mine01`, **3 of 14** level lights reached both sides of door 0
  door-blind; **0** do now.
- **The headlight claims a shadow slot first.** It was claimed after every
  visible level light, so with eight of those on screen it cast nothing and
  lit through walls and robots. Level lights are now taken nearest first, and
  the default budget is 16.
- **Door slabs are oversized by 0.25 units into the frame** so no face is
  exactly coplanar with the adjacent wall, and they are drawn and cast at
  every openness rather than vanishing at 0.75.

### Verified (Linux container, software GL)

| Check | Result |
| --- | --- |
| A door at 0.04 open | ✅ see-through, still blocks a body, light reaches through |
| Level lights vs shut door 0 | ✅ **0 of 14** reach both sides (3 door-blind) |
| Portal walk fixture (world test) | ✅ opens and closes on the `opaque` flag |
| World, loop, weapons, robots tests on `mine01` | ✅ PASS |
| World test on `test01` | ✅ PASS |
| Headless render from both sides of door 0 | ✅ no GL error or fatal lines, 111 steps/s at 640×360 |
| Shadow budget order and the 16 default | ⚠️ not measurable here; needs the dev machine's overlay and frame rate |

---

## Fixes after M7 — self-shadow fizz on meshes

Reported from play: the ship showed strange shadow artifacts in the chase
camera. The same artifact was on every robot, pillar and door slab.

The M3 rule that the volume's front cap must be bit-identical to the depth
prepass was only applied to the mine, whose model matrix is the identity.
Mesh occluders were transformed to world space on the CPU and drawn with an
identity matrix, which computes the same point by a different association;
the cap landed an ulp in front of the surface, was not counted, and the
surface shadowed itself. Volumes are now emitted in each occluder's own space
and drawn per occluder with the model matrix the prepass used.

### Verified (Linux container, software GL)

| Check | Result |
| --- | --- |
| Pillar under the headlight, segment 12, before | ❌ almost entirely black with fizzing edges |
| Same view after | ✅ fully lit |
| Chase-camera ship, before / after | ❌ speckled hull → ✅ clean |
| World test volume closure, 14 `mine01` and 8 `test01` lights | ✅ PASS (the check now runs on object-space output) |
| World, loop, weapons, robots tests on `mine01` | ✅ PASS |
| Headless renders | ✅ no GL error or fatal lines |

---

## M8 — Polish and performance 🟡 In progress

Landing in pieces. The automap is done; muzzle flash and hit feedback,
performance instrumentation, asset validation and the README follow.

### The automap ✅

`Tab` draws the segment graph as lines from an orbit camera the mouse drives,
showing only what the player has earned the right to see. The simulation keeps
running behind it.

Discovery is recorded in `GameState` and nothing reads it back, so a level plays
out identically whether the map is opened or not.

| Check | Result |
| --- | --- |
| Only the start segment is known at the start | ✅ `visitedCount == 1`, and it is `level.start.segment` |
| Every other segment stays unknown | ✅ 33 of 34 on `mine01` |
| Visiting N segments marks exactly N | ✅ walked 6, `visitedCount == 6` |
| A key in an unvisited room is not on the map | ✅ not `seen` until the room is entered |
| The reactor is not on the map from the start | ✅ needs a real line of sight |
| Sighting the reactor records its position | ✅ within 1.0 u of the reactor, at 20.0 48.0 70.0 |
| The map draws the whole graph | ✅ `--reveal` screenshot, 34 segments, three keys and the reactor marked |
| The map draws only the start room at the start | ✅ `--automap` screenshot: one room, frontier portals faint |
| The simulation keeps running while it is open | ✅ 92 sim steps over 40 frames with the map up |
| Debug run, GL errors fatal | ✅ no GL error or fatal lines |
| Loop, world, weapons, robots, collision, flight on both levels | ✅ PASS (loop SKIPPED on `test01`) |
| `--resize-test` | ✅ completes, 0 frames skipped while minimized |

Two notes on what these numbers are worth. The step rates in the container are
software-rendering numbers and the full-size ones are bounded by the loop's
0.25 s frame clamp, not by the simulation — 119.4/120 at 640x360 is the real
check. And the reveal screenshot proves the map *renders* the graph; the loop
test, not the screenshot, is what proves it only reveals what was found.

### Feel: muzzle flash, hit flinch, missile kick ✅

Three effects that exist to be felt. Each lands on `RenderState` or `HudData`
and never on the body, and each test asserts that.

| Check | Result |
| --- | --- |
| Every weapon throws a flash carrying a light | ✅ 4 of 4 |
| The flash is brighter than the bolt it launched | ✅ laser 6.5 vs 2.6, vulcan 3.8 vs 1.5, concussion and homing 7.5 vs 3.0 |
| The flash is out again within 0.2 s | ✅ (counted near the ship — by then the bolt's impact flash is lit too, at the wall) |
| Robot fire flashes as well | ✅ caught on the step a gunner fires |
| Being hit sets the flinch | ✅ through the real damage path, a missile's own splash off the floor |
| The flinch does not move the ship | ✅ position and velocity unchanged |
| The flinch fades | ✅ zero after 0.45 s |
| A missile kicks, a gun does not | ✅ laser leaves `recoilTimer` at zero |
| The kick does not push the ship | ✅ velocity unchanged |
| The tint draws with the HUD hidden | ✅ screenshot under `--no-ui` after robots landed a hit |
| All tests, both levels, both configs | ✅ PASS (loop SKIPPED on `test01`) |
| `--resize-test`, Debug, GL errors fatal | ✅ completes, no GL error or fatal lines |
| Step rate, 640x360, 500 frames | **120.0 / 120** |

What these numbers do **not** cover: whether the flash actually reads as
firing-to-see in a dark corridor. That is an aesthetic judgement at a real frame
rate, and software rendering at ~13 fps cannot make it. The same goes for the
final tint smoothing — the screenshot that caught a hit was of the earlier
four-band version; going to eight bands changed only the band count and the
per-band alpha, and I did not land another hit frame to re-capture it. Both want
the dev machine.

### Performance instrumentation ✅ (measurement), ⬜ (the target itself)

GPU timestamp queries per pass, and an allocation counter, both reported in an
ImGui panel and on a line at exit so a headless run can see them.

| Check | Result |
| --- | --- |
| Timestamp queries resolve | ✅ read back two frames late, no pipeline stall |
| Per-pass breakdown, `mine01`, 640x360 | prepass **3.67 ms**, stencil **22.11**, lighting **3.03**, unlit **0.05**, total **28.87 ms** over 16 lights |
| Most expensive light identified | ✅ light 4 at **4.53 ms** |
| Automap view timed | ✅ **1.64 ms** unlit, everything else zero — the map is ~18x cheaper than the scene |
| Sim step allocations, steady state | ✅ **0** on 269 of 270 frames |
| Draw path allocations, steady state | ✅ **0** on 269 of 270 frames |
| Negative control: one `std::vector<int>(4)` per sim step | ✅ caught — "SOME on 89 of 90 frames (worst 17)", draw path still clean |
| All tests, both levels, both configs | ✅ PASS (loop SKIPPED on `test01`) |
| `--resize-test` | ✅ completes, no GL error or fatal lines |
| Step rate, 640x360, 300 frames | **119.9 / 120** |

The three allocations that are not zero were each traced, and none is per-frame:
the death message being formatted by `log::info` inside `stepGame`; the render
target's log line on the frame a resize rebuilds it; and a per-frame container
reaching a new high-water mark, which happens once and not again — the signature
of capacity growth rather than churn. I did not chase which container.

**The 200 fps at 1440p target is not measured and cannot be here.** These numbers
come from zink over lavapipe at 640x360 — software rasterization, where a
fill-heavy stencil pass is pathologically slow. Stencil taking 77% of the frame
is a property of the software rasterizer, not a prediction about the 4070 Ti, and
anyone reading it as one will optimize the wrong thing. The instrument is what is
verified here; the numbers are not a benchmark.

Also deliberately not done: the per-light volume triangle **cap** M8's plan
suggested. A truncated volume is an open volume, and depth-fail against an open
volume is exactly the bug the shadow-closure work fixed. `worstVolumeTriangles`
reports the number instead, so a cap can be sized from evidence if evidence ever
appears. The cheap half of that suggestion was already in place: a light whose
segment set misses the visible set does no silhouette work at all.

Tracy is the agreed follow-up for the deep profiling, added on the dev machine
where it can be confirmed working.

### Asset validation and the README ✅

Every shipped model and level through the M2 and M3 validators at startup and
behind `--validate-assets`, one line each.

| Check | Result |
| --- | --- |
| All shipped assets validate | ✅ **0 failures** — 6 models and 2 levels |
| Level surfaces are closed genus-0 | ✅ `V - E + F = 2` on both `mine01` and `test01` |
| Negative control: a face deleted from `pillar.obj` | ✅ caught — "3 open edge(s)", first at (1.700 9.000 0.000) |
| Debug refuses to continue past it | ✅ `[FATAL] aborting` |
| Release reports and skips it | ✅ "skipping ... because this is a Release build", exit code **1** |
| Restored asset | ✅ exit code **0** |
| Runs at startup, before a window exists | ✅ `--resize-test` logs `asset validation: 0 failure(s)` |
| All tests, both levels, both configs | ✅ PASS (loop SKIPPED on `test01`) |

One deliberate departure from the plan: a failing **level** is fatal in Release
too, not skipped. Skipping a model costs a pillar; skipping a level leaves
nothing to play, so stopping and saying why is the better outcome. Models behave
as the plan asks.

`README.md` covers what the project is, the three permanent bets, both build
paths, the controls, how to verify it, and credits — Descent for the mine and
the flight model, Doom 3 for the shadow volume implementation and depth-fail,
F.E.A.R. for the argument that hard shadows with no ambient term are worth
committing to. The screenshots are a placeholder: software rendering at ~13 fps
cannot show the technique at a frame rate that flatters it, so those want the
dev machine.

### Housekeeping done on the way

`src/game/bindings.h` now holds the discrete keys too — F1, F3, H, F, Tab,
Escape and the weapon-select base. They had been GLFW constants spelled out in
`main.cpp`, which made CLAUDE.md's "nothing else in the project names a GLFW key
code for gameplay" false as written. There is now no `GLFW_KEY_` anywhere in
`main.cpp`.

---

## Rendering follow-up — shadow closure and portal revisits

Investigated black polygon artifacts that disappear when individual lights stop
casting shadows, and rooms that remain black through an opened doorway.

- Mine shadow volumes now use the entire closed mine surface. The earlier
  segment-subset optimization left openings in emitted volumes; radius falloff
  cannot protect depth-fail counts from missing geometry behind a receiver.
- Portal traversal now revisits a room when another route expands its screen
  window, rather than assuming that a shallow graph path already saw every exit.
  Coverage is cleared every frame, so closing a door cannot retain old visibility.
- The world test checks oriented edge cancellation in actual emitted shadow
  triangles and includes a door opening onto a second route into the same room.

### Verified on Windows / RTX 4070 Ti

- The new portal fixture failed before the traversal fix (the opened door did
  not reveal the exit), then passed opening and closing without camera movement.
- Generated shadow volumes are closed for all 14 `mine01` lights and all eight
  `test01` lights. World tests pass in Debug and Release; culling still limits
  the sampled visible sets to 14/34 and 10/14 segments respectively.
- World, loop, weapons, robots and flight tests pass in both configurations on
  both levels (`test01` skips the loop test because it has no game flow).
- Debug and Release build successfully. The existing C4456 warning in
  `Renderer::buildBillboards` remains. Debug verification used a separate
  `build/verify` directory because the user's original Debug game was running.
- Before/after start-room screenshots show corrected lighting around the door.
  The exact user camera pose was not reproduced; this is not an end-to-end
  confirmation of every reported doorway artifact.
- Debug resize/iconify/restore: 1797 frames, 119.8 simulation steps/s, no GL
  error/fatal messages; 50 minimized frames excluded from timing.
- Debug render probes from all 34 segment centres: three frames each, no
  process failures or GL error/fatal messages. These are smoke checks, not
  image-based assertions of shadow correctness.
- Release smoke run: 1000 frames in 3.174 s (~315 fps), framebuffer 2560x1421,
  119.7 simulation steps/s. This is a start-room sample, not the M8 benchmark.

### Still outstanding

The collision issue remains outside this rendering fix. A freshly rebuilt
Release executable reports segment **16**, direction (-0.71, 0, -0.71): raycast
71.023 units versus marching 13.850, one disagreement in 884 rays. This differs
from the earlier segment-24 diagnosis above, including the direction of error;
the old claim that the remaining ray always stops early needs re-investigation.
No collision, projectile, or gameplay door-blocking behavior was changed.

That re-investigation is done: the call for it was the right instinct, and the
answer was that **neither number described a collision bug**. See the section
below.

---

## Fixes after M7 — the raycast oracle

M7's one outstanding failure, carried since the milestone was paused, was never in
`src/sim`. `--collision-test` checked `sim::raycast` against a marching oracle,
and it was the oracle that was wrong.

The ray it flagged, from segment 16's centre on `mine01` along (−0.71, 0, −0.71),
passes from segment 16 into segment 14 through a portal and **grazes the portal
corner with 0.0202 units of clearance**, still inside the mine, with clearance
climbing back to 0.11, 0.29 and 0.47 over the next three steps. The oracle
stopped there because M7 had given it a "stay at least one 0.05-unit step clear
of the surface" rule — added to stop it marching *through* the point where two
segments meet at a grid diagonal, which was the earlier segment-24 report. One
guard, two opposite false results, and that is why the recorded diagnosis flipped
direction between runs.

`raycast` was never consulted about objects or robots, so the robot spawn-point
fix recorded above could not have been involved either: the query takes wall
triangles, portals and shut doors, and nothing else.

### The fix

The marching oracle is replaced by an exact one: test the ray against every wall
triangle in the level and keep the nearest hit. The union of all wall triangles is
one closed surface (M3 validates that), so the first triangle the ray meets *is*
the wall it hits. It walks no segment graph, which is what keeps it independent of
`raycast`, and it has no threshold to tune against grazing angles. 244 triangle
tests per ray on `mine01`.

The tolerance drops from **0.35 units to 0.001**, which is float noise rather than
a fudge factor.

### Verified (Linux container, software GL; these tests need no window and no GL)

| Check | Result |
| --- | --- |
| `--collision-test` on `mine01`, Debug and Release | ✅ **PASS** — 884 rays, 0 missed, **0 disagreements**, worst delta **0.000011 u** |
| `--collision-test` on `test01`, Debug and Release | ✅ **PASS** — 364 rays, worst delta **0.000011 u** |
| Containment, sliding, portals, sweeps, unchanged | ✅ 884 trials 0 escapes, 122 walls 0 caught, 82 crossings 0 failed, 884/884 sweeps |
| Negative control: `raycast` perturbed by **0.01 u** | ✅ caught on **all 884 rays** — 35× below what the old 0.35 tolerance could see |
| World, weapons, robots, loop, flight tests on `mine01` | ✅ PASS |
| Same on `test01` | ✅ PASS (loop SKIPPED, no doors or exit) |
| Debug and Release build | ✅ clean, no new warnings |

The collision, projectile and door-blocking code is untouched: the only change is
`src/game/collision_test.cpp`.

### What this closes

M7's "not done yet" list loses its one failure. What remains for the milestone is
the `ARCHITECTURE.md` work for doors, the HUD and the game flow, and playing
`mine01` end to end on the dev machine — key routing, the 45-second countdown,
and whether 16 robots is the right number. None of that can be settled in the
container.

---

## M7 documentation — the game loop and the HUD

The last of M7's paperwork. `docs/ARCHITECTURE.md` gains two sections, written
against the code rather than from the milestone prompt:

- **The game loop** — `GameState` as trivially copyable data inside `SimState`,
  the four phases and the one direction through them, the reactor as a robot
  with an `isReactor` flag rather than a special object type, the shake as a
  `RenderState` offset read from wall-clock so it never moves the ship or banks
  motion across a stall, and why a locked door does not twitch.
- **The HUD** — `HudData` as a flat struct the game layer fills, so `src/render`
  stays free of simulation types; the 37-glyph 5x7 font generated into an atlas
  at startup instead of adding a font dependency; and the two state changes
  (culling and depth test) that silently draw nothing when left on.

Doors were already documented when they were fixed, so that part of the list was
already closed.

### Verified

Docs only — no source change, so nothing to re-measure. Checked that every claim
in the new text matches the code, which caught two of my own: `GameState` is not
pointer-free (`endReason` points at string literals), and the shake reads
`clock.seconds()` rather than the simulation's elapsed time. Both corrected
before commit.

### What is left in M7

Playing `mine01` end to end on the dev machine: whether the key routing reads,
whether 45 seconds is the right countdown, and whether 16 robots is too many or
too few. That is a judgement call at a keyboard, not something the container can
answer.
