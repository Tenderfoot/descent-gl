# descent-vk

A recreation of Descent's core gameplay: 6DOF flight through a mine, robots,
reactor, escape. C++20 and OpenGL 4.6 core, lit the way F.E.A.R. and Doom 3
were — no ambient term, full blacks, hard stencil-volume shadows. No sound.

The repository name says `vk` for historical reasons; the renderer is OpenGL.
See `docs/ARCHITECTURE.md` for why.

## Build

Prerequisites already present on this machine: Visual Studio 2026 Community
(MSVC 14.51), CMake 4.3, Ninja, git, and the vcpkg bundled with Visual Studio.
No Vulkan SDK and no separate vcpkg clone are needed.

```
cmake --preset local
cmake --build --preset local-debug
cmake --build --preset local-release
```

### Linux dev container

The shipping target is Windows/MSVC. A Linux container can still build and
smoke-run the game, which is how an agent without the dev machine verifies a
milestone:

```
sudo tools/linux-deps.sh          # apt packages, glad, imgui into .deps/
cmake --preset linux
cmake --build --preset linux-debug
tools/run-headless.sh --frames 120 --screenshot /tmp/shot.png
```

Dependencies are the same allowlist, resolved from distribution packages plus
`.deps/` instead of vcpkg (`cmake/LinuxDeps.cmake`). GL goes through zink over
lavapipe, because Mesa's llvmpipe advertises only GL 4.5 and this project asks
for 4.6 core. It is software rendering: tens of fps, not thousands. Treat it as
a correctness check, never as a performance measurement.

`CMakePresets.json` is committed and resolves vcpkg from `$env{VCPKG_ROOT}`.
`CMakeUserPresets.json` is machine-local and git-ignored; it defines the
`local` preset that points `VCPKG_ROOT` at the VS-bundled vcpkg. A fresh clone
on another machine either sets `VCPKG_ROOT` and uses the `vs` preset, or writes
its own user preset.

First configure takes a few minutes while vcpkg builds dependencies;
afterwards they come from the binary cache in seconds.

## Run

```
build\Debug\descent.exe
build\Release\descent.exe
```

Shaders and assets are loaded relative to the executable and copied next to it
by a post-build step, so run from anywhere.

### Verification flags

The app can be driven without a person at the keyboard. Use these to verify a
milestone rather than describing what the code should do:

| Flag | Effect |
| --- | --- |
| `--frames N` | Exit after N rendered frames. Disables vsync so the run is not pinned to the refresh rate. |
| `--screenshot PATH` | Write the final frame to PATH as PNG before exiting. Implies `--frames 60` if no count is given. |
| `--resize-test` | Drive a scripted 9-second sequence of window resizes, an iconify and a restore, then exit. Keeps vsync on so the window manager can deliver the events. |
| `--chase` | Start in the chase camera, so a screenshot can show the ship without a keypress. |
| `--flight-test` | Run the flight model headlessly, report spin-up, coast-down and swipe-to-turn against their targets, and exit non-zero if any is out of range. Needs no window and no GL. |
| `--world-test` | Load and validate the level, then report portal visibility, light reach and brightness from every segment. Exits non-zero if the cull is a no-op, a light misses its own segment, or no segment is dark. Needs no window and no GL. |
| `--segment N` | Start in segment N's centre rather than at the player start, so a screenshot can be taken anywhere in the mine without flying there. |

Every run ends with a line reporting frames, simulation steps, and the achieved
step rate against the 120 Hz target. That rate is the check that the fixed
timestep is real: it must sit on 120 regardless of how fast frames were drawn.
Time spent minimized and the final screenshot encode are both excluded from it.

## Controls

| Input | Action |
| --- | --- |
| Mouse | Pitch and yaw (only while captured) |
| W / S | Forward / back |
| A / D | Strafe left / right |
| Space / Left Ctrl | Up / down |
| Q / E | Roll left / right |
| Arrow keys | Pitch and yaw without the mouse |
| F1 | Toggle chase camera |
| Escape | Toggle mouse capture |

All of these come from one table in `src/game/bindings.h`; nothing else in the
project names a GLFW key code for gameplay.

## Conventions

- **Plain structs and free functions.** No ECS, no render graph, no job system,
  no abstraction layer over GL beyond small RAII wrappers. Keep it direct.
- **`src/core`** window, input, time, logging, options. Knows nothing about GL.
- **`src/render`** everything OpenGL. The only place that includes `glad`, and
  it does so through `render/gl.h`.
- **`src/sim`** physics, collision, projectiles, AI. Must not touch GL.
- **Level data lives in `src/game/level.h`** and is read by both `sim` and
  `render`. It is data, not logic, so both may depend on it; the rule that
  matters is that `sim` never touches GL.
- **`src/game`** game state and app-level glue.
- **No per-frame heap allocation** in the sim step or the draw path.
- **Fixed 120 Hz simulation** with an accumulator; rendering interpolates
  between the two most recent states. `SimState` and `RenderState` are separate
  types on purpose.
- **GL errors are fatal in Debug.** Any `GL_DEBUG_SEVERITY_HIGH` message from
  `KHR_debug` aborts. This is the project's equivalent of treating a Vulkan
  validation error as fatal.

## Dependencies

Allowlist, all from vcpkg: `glfw3`, `glad`, `glm`, `imgui` (glfw and opengl3
bindings), `stb`, `nlohmann-json`, `tinyobjloader`.

**Ask before adding anything else.**

## Before you finish a milestone

1. `cmake --build --preset local-debug` and `--preset local-release` both clean.
2. Run both configurations; no `[ERROR]` or `[FATAL]` lines.
3. Run `--resize-test`; it completes and reports a step rate on 120.
4. Update `docs/ARCHITECTURE.md` with whatever the milestone introduced.
5. Add the milestone to `docs/MILESTONES.md` with what was actually verified,
   including real numbers.
6. Commit.
