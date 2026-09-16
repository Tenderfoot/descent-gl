# descent-vk

A recreation of Descent's core gameplay: 6DOF flight through a mine, robots,
reactor, escape. C++20 and OpenGL 4.6 core, lit the way F.E.A.R. and Doom 3
were — no ambient term, full blacks, hard stencil-volume shadows. Positional
sound effects, no music.

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
| `--automap` | Start with the automap open, so a screenshot can show it without flying the level first. |
| `--validate-assets` | Run every shipped model and level through the M2 and M3 validators, print one line each, and exit non-zero on any failure. Needs no window and no GL. Also runs at every startup. |
| `--reveal` | Shade the automap as if every segment had been visited and draw every key and the reactor as if found. The whole mine is drawn either way; this only changes which parts read as already flown. A rendering check for the map, in the same family as `--wireframe-volumes`; it changes nothing in the simulation, which still records only the rooms actually flown through. Implies `--automap`. |
| `--flight-test` | Run the flight model headlessly, report spin-up, coast-down and swipe-to-turn against their targets, and exit non-zero if any is out of range. Needs no window and no GL. |
| `--world-test` | Load and validate the level, then report portal visibility, light reach and brightness from every segment. Also drives the level's moving props: every floating light is shoved at 150 u/s in all 26 directions and must bounce and stay inside the mine, and every swinging lamp must reach its authored amplitude with its cord length held. Exits non-zero if the cull is a no-op, a light misses its own segment, or no segment is dark. Needs no window and no GL. |
| `--segment N` | Start in segment N's centre rather than at the player start, so a screenshot can be taken anywhere in the mine without flying there. |
| `--size W H` | Window size. Mostly for software-rendering verification: at full size a shadowed frame can be slower than the loop's 0.25 s frame clamp, and the reported step rate is then bounded by the clamp rather than by the simulation. |
| `--collision-test` | Fly the ship into every wall from all 26 face, edge and corner directions at max speed and check it never escapes; cross-check the ray and sweep queries against an independent method; walk every portal both ways. Exits non-zero on any failure. Needs no window and no GL. |
| `--weapons-test` | Fire every weapon at walls and at targets and check what happened: rate of fire, energy and ammo, wall impacts, a 2000 u/s no-tunnelling case, target destruction, splash, flares, headlight drain and death reset. Needs no window and no GL. |
| `--loop-test` | Play the level's win and lose conditions through: keys against doors, a shut door against light, the reactor, the countdown, the exit and every pickup. Reports SKIPPED on a level with no doors or exit. Needs no window and no GL. |
| `--light-test` | Run the CPU illumination query headlessly: the falloff against the shader's own formula, the spot cone, the segment walk against brute-force ray/triangle over every wall, and whether the level has both lit and dark places to stand. Needs no window and no GL. |
| `--headlight` | Start with the headlight on. It is off by default, so a screenshot cannot otherwise show it without a keypress. |
| `--level PATH` | Load a level other than the default `mine01`. |
| `--debug-ui` | Start with the ImGui debug panels open. They are off by default from M7; F3 toggles them at runtime. |
| `--robots-test` | Drive the robot state machine through idle, alert, chase, attack, retreat and death; check pathing crosses portals, that robot fire hurts, that deaths drop, and that no robot leaves the mine. Needs no window and no GL. |
| `--autofire W` | Hold the trigger for weapon W (0 laser, 1 vulcan, 2 concussion, 3 homing, 4 railgun, 5 lightning, 6 flares), so a screenshot can show bolts and beams in flight and the lights they carry. |
| `--no-ui` | Hide the ImGui panels, so a screenshot shows the game rather than the debug windows. |
| `--debug-lines` | Start with wall normals, contact points and the nose raycast drawn. Also ImGui toggles. |
| `--wireframe-volumes` | Start with shadow volumes drawn as wireframe. Also an ImGui toggle. |
| `--stencil-view` | Start in the stencil visualiser, which tints whatever the stencil marks as shadowed. Also an ImGui toggle. |
| `--sound-test` | Load every shipped clip and run the mixer headlessly: attenuation over distance, the stereo image, the voice pool under pressure, whether a loop loops, whether the bus clips, and whether the mix path allocates. Needs no window, no GL and no sound card. |
| `--no-sound` | Do not open a playback device. |
| `--volume V` | Master volume, 0..1. Default 0.8. |
| `--start-menu` | Show the player main menu even during a frame-limited or screenshot run. |
| `--level-select` | Open the main menu on the level list, so a screenshot can show it without navigating there. Implies `--start-menu`. |
| `--level-switch-test` | Drive the level list to a second level, load it, fly it, and report whether the switch took. Exits non-zero if the new level is not loaded and playing. Needs a window; its step rate is below 120 by construction, because the first twenty frames are spent in the menu. |
| `--settings-menu` | Open the player settings screen, so its layout can be captured without navigating there. |
| `--settings-test` | Exercise missing, partial, malformed, saved and reloaded player settings without a window. |
| `--settings PATH` | Read and write player settings at a specific path instead of the platform user configuration directory. |
| `--controls-menu` | Open the keyboard and mouse binding screen for visual verification. |
| `--progress-test` | Exercise run records, best times, malformed data and atomic persistence without a window. |
| `--progress PATH` | Read and write run history at a specific path instead of the platform user configuration directory. |

Every run also ends with the last frame's GPU cost per pass and a report of how
many heap allocations the sim step and the draw path made after a 30-frame
warm-up. Both should read `none`; a number there is a finding, not noise.

A run also reports what it heard: every sound cue the simulation raised, with
its count, and how many events it had to drop. A cue that never fires is the
interesting case — it means either the level never reached that situation or
nothing emits it — so the line lists cues rather than a total. A container with
no sound card reports `device "none"` and is not a failure; the events are still
counted, because they come from the simulation and not from the device.

Every run ends with a line reporting frames, simulation steps, and the achieved
step rate against the 120 Hz target. That rate is the check that the fixed
timestep is real: it must sit on 120 regardless of how fast frames were drawn.
Time spent minimized and the final screenshot encode are both excluded from it.

## Controls

A normal launch opens the player main menu. Any command-line run enters the
level directly unless `--start-menu` is present.

| Input | Action |
| --- | --- |
| Mouse | Pitch and yaw (only while captured) |
| W / S | Forward / back |
| A / D | Strafe left / right |
| Space / Left Ctrl | Up / down |
| Q / E | Roll left / right |
| Arrow keys | Pitch and yaw without the mouse |
| F1 | Toggle chase camera |
| F3 | Toggle the ImGui debug panels |
| Tab | Toggle the automap (mouse orbits it; the simulation keeps running) |
| 1 / 2 / 3 / 4 | Select laser, vulcan, lightning, railgun |
| G | Cycle the secondary weapon (concussion, homing) |
| Left mouse | Fire the selected gun. The railgun fires one traced shot per pull; the lightning gun runs for as long as it is held. |
| Right mouse | Launch a missile |
| F | Throw a flare |
| H | Toggle the headlight (off at the start) |
| M | Mute |
| Escape | Pause / resume |

Player actions and their defaults come from `src/game/input_actions.cpp`.
`src/game/bindings.h` samples the current persisted keyboard and mouse bindings
into simulation controls; gameplay call sites identify actions, not GLFW codes.

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

Plus one vendored, committed under `third_party/`: **miniaudio** (single header,
public domain). It is not on vcpkg and has no usable Debian package, and it has
to exist on both build paths, so it is in the repository rather than fetched.
It needs no new apt package on Linux — it `dlopen`s ALSA and PulseAudio at
runtime.

**Ask before adding anything else.**

## Before you finish a milestone

1. `cmake --build --preset local-debug` and `--preset local-release` both clean.
2. Run both configurations; no `[ERROR]` or `[FATAL]` lines.
3. Run `--resize-test`; it completes and reports a step rate on 120. Under
   software rendering it will not: the script forces 1920x1080, a shadowed
   frame there costs more than the loop's 0.25 s clamp, and the rate is then
   bounded by the clamp. Compare against the same run on `main` rather than
   against 120.
4. Run every headless test: `--validate-assets --flight-test --world-test
   --collision-test --weapons-test --robots-test --loop-test --sound-test
   --light-test --settings-test --progress-test`.
5. Update `docs/ARCHITECTURE.md` with whatever the milestone introduced.
6. Add the milestone to `docs/MILESTONES.md` with what was actually verified,
   including real numbers.
7. Commit.
