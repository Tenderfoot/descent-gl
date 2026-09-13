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

## M1 — 6DOF flight ⬜

Not started.
