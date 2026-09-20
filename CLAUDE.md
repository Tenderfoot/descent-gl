# descent-vk

A recreation of Descent's core gameplay: 6DOF flight through a mine, robots,
reactor, escape. C++20 and OpenGL 4.6 core, lit the way F.E.A.R. and Doom 3
were — no ambient term, full blacks, hard stencil-volume shadows. Positional
sound effects and an adaptive stereo combat score.

The repository name says `vk` for historical reasons; the renderer is OpenGL.
See `docs/ARCHITECTURE.md` for why.

## Scope of a change

Implement what was asked, then stop. Unless the request names one of these,
it is out of scope:

- **No documentation updates.** `docs/ARCHITECTURE.md` and
  `docs/MILESTONES.md` get written when they are asked for, in a session for
  that purpose — not as a tail on every change.
- **No building or running.** Do not invoke CMake, the compiler, or the game.
- **No new tests and no new headless oracles.**
- **No new launch options.** There are already more than forty. The bar for
  another is that someone asked for it.
- **No verification narration.** Report what changed, not what was checked.

Every one of these is welcome when the request asks for it by name. The
default is that it is not part of the job.

## Build

Prerequisites on the dev machine: Visual Studio 2026 Community (MSVC 14.51),
CMake 4.3, Ninja, git, and the vcpkg bundled with Visual Studio. No Vulkan
SDK and no separate vcpkg clone are needed.

```
cmake --preset local
cmake --build --preset local-debug
cmake --build --preset local-release
```

`CMakePresets.json` is committed and resolves vcpkg from `$env{VCPKG_ROOT}`.
`CMakeUserPresets.json` is machine-local and git-ignored; it defines the
`local` preset that points `VCPKG_ROOT` at the VS-bundled vcpkg. A fresh
clone on another machine either sets `VCPKG_ROOT` and uses the `vs` preset,
or writes its own user preset.

A Linux container can also build and smoke-run the game, through the `linux`
preset and `tools/linux-deps.sh`. GL there goes through zink over lavapipe,
so it is software rendering: a correctness path, never a performance
measurement.

## Run

```
build\Debug\descent.exe
build\Release\descent.exe
```

Shaders and assets are loaded relative to the executable and copied next to
it by a post-build step, so run from anywhere.

A normal launch opens the player main menu. Any command-line run enters the
level directly unless `--start-menu` is present.

The app takes a large set of command-line options so it can be driven
without a person at the keyboard — frame limits, screenshots, scene setup,
and a family of headless `--*-test` checks. **`src/core/options.h` is the
list.** Read it there when a flag is needed. It is deliberately not repeated
here; a second copy only drifts from the first.

Controls live in `src/game/input_actions.cpp`, which holds every player
action and its default binding. `src/game/bindings.h` samples the current
persisted keyboard and mouse bindings into simulation controls; gameplay
call sites identify actions, not GLFW codes.

## Conventions

- **Plain structs and free functions.** No ECS, no render graph, no job system,
  no abstraction layer over GL beyond small RAII wrappers. Keep it direct.
- **`src/core`** window, input, time, logging, options. Knows nothing about GL.
- **`src/render`** everything OpenGL. The only place that includes `glad`, and
  it does so through `render/gl.h`.
- **`src/sim`** physics, collision, projectiles, AI. Must not touch GL, and
  must not touch audio either: it reports what happened into a
  `sim::EventQueue` (`src/sim/events.h`) and something else makes the noise.
- **`src/audio`** WAV loading, mixing, 3D positioning, the device. The only
  place that includes `miniaudio.h`, and it does so through `audio/device.cpp`.
  Knows nothing about GL.
- **`sim::PropState`** (`src/sim/props.h`) is simulation state for things that
  move but are not ships: the floating lights and the swinging lamps. Kept
  apart from `Level::objects`, which is authoring data and assumes nothing ever
  leaves the position it was placed at.
- **Level data lives in `src/game/level.h`** and is read by both `sim` and
  `render`. It is data, not logic, so both may depend on it; the rule that
  matters is that `sim` never touches GL.
- **`src/game`** game state and app-level glue.
- **No per-frame heap allocation** in the sim step, the draw path, or the audio
  callback. The callback has a hard deadline: an allocation there is an audible
  click, not a slow frame. It talks to the game thread through a lock-free ring
  and never blocks.
- **Fixed 120 Hz simulation** with an accumulator; rendering interpolates
  between the two most recent states. `SimState` and `RenderState` are separate
  types on purpose.
- **GL errors are fatal in Debug.** Any `GL_DEBUG_SEVERITY_HIGH` message from
  `KHR_debug` aborts. This is the project's equivalent of treating a Vulkan
  validation error as fatal.

## Dependencies

Allowlist, all from vcpkg: `glfw3`, `glad`, `glm`, `imgui` (glfw and opengl3
bindings), `stb`, `nlohmann-json`, `tinyobjloader`, `lua`.

Plus two vendored, committed under `third_party/`:

- **miniaudio** (single header, public domain). It needs no new apt package on
  Linux — it `dlopen`s ALSA and PulseAudio at runtime.
- **pl_mpeg** (single header, MIT). MPEG-1 video for the HUD comm panel; only
  `src/render/comm_video.cpp` includes it.

Neither is on vcpkg or has a usable Debian package, and both have to exist on
both build paths, so they are in the repository rather than fetched.

**Ask before adding anything else.**
