# descent-gl

A recreation of Descent's core loop — 6DOF flight through a mine, robots,
reactor, escape — in C++20 and OpenGL 4.6 core, lit the way F.E.A.R. and Doom 3
were: **no ambient term, full blacks, and hard stencil-volume shadows from every
light**.

A surface no light reaches is `(0,0,0)`. That is not a colour grade — it is the
absence of a lighting pass. Darkness is the mechanic the rest of the game hangs
off: the headlight, the flares, and firing to see what is in the room.

Every gun, robot, door and explosion is a point in the mine, panned and
attenuated against the ship's own basis — roll inverted and a sound on your
left moves to your right. A stereo adaptive score crossfades from an ominous
lull into combat as robots acquire the player, and ducks under transmissions.

> The repository is named `descent-gl` and the project inside it calls itself
> `descent-vk`. It was planned for Vulkan and switched to OpenGL 4.6 before any
> code existed; the name stayed rather than churn every path and import.
> `docs/ARCHITECTURE.md` has the reasoning.

## Screenshots

<!-- TODO: a lit corridor, a shadow thrown by a robot under the headlight, the
     automap, and the countdown. Taken on the dev machine at 1440p, since the
     Linux container renders in software and cannot show the technique at a
     frame rate that flatters it. -->

_To come._

## Three permanent bets

Everything else follows from these, and they are marked as permanent in the docs
so a later session does not quietly simplify one away.

1. **Levels are segment graphs.** A mine is a graph of deformed cubes, each with
   six sides that are either a textured wall or a portal into a neighbour. One
   structure gives visibility (a portal walk), collision (six plane tests),
   pathfinding (breadth-first search) and light culling (segment sets).
2. **No ambient light.** A depth prepass, then one additive pass per light.
   Nothing else writes colour.
3. **Stencil shadow volumes.** Silhouette edges extruded to infinity and counted
   with depth-fail. Pixel-exact hard edges, no shadow acne, no resolution to
   pick. It needs closed meshes, which the ship and robots are by construction
   and the mine is because segments share vertices across portals.

## Build

### Windows (the shipping target)

Visual Studio 2022 or newer with the C++ workload, CMake 3.25+, and the vcpkg
that ships with Visual Studio. No Vulkan SDK and no separate vcpkg clone.

```
cmake --preset local
cmake --build --preset local-debug
cmake --build --preset local-release
```

`CMakePresets.json` resolves vcpkg from `$env{VCPKG_ROOT}`. A fresh clone either
sets that and uses the `vs` preset, or writes its own git-ignored
`CMakeUserPresets.json`.

### Linux (development and CI checks)

Builds and smoke-runs the game, which is how a change is verified without the
dev machine.

```
sudo tools/linux-deps.sh
cmake --preset linux
cmake --build --preset linux-debug
tools/run-headless.sh --frames 120 --screenshot /tmp/shot.png
```

GL goes through zink over lavapipe, because Mesa's llvmpipe advertises only GL
4.5 and this project asks for 4.6 core. It is software rendering: tens of frames
per second, not thousands. **Treat it as a correctness check, never as a
performance measurement.**

## Controls

A normal launch opens the main menu. Command-line verification runs continue
to enter the selected level directly.

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
| 1 / 2 / 3 / 4 | Select laser, vulcan, lightning or railgun for primary fire |
| G | Cycle the secondary weapon: concussion, homing |
| Left mouse | Fire the selected primary weapon |
| Right mouse | Fire the selected secondary weapon |
| F | Throw a flare |
| H | Toggle the headlight |
| Escape | Pause / resume |

Every one of these is a default in a single table in
`src/game/input_actions.cpp`, and every one can be rebound from the controls
screen. `src/game/bindings.h` samples whatever is bound into the controls the
simulation reads.

## Verifying it

The game can be driven without a person at the keyboard, which is how each
milestone was checked. The headless tests need no window and no GL, so they run
anywhere:

```
descent --collision-test     # 884 containment trials, sweeps, and the ray query
descent --weapons-test       # fire rate, ammo, tunnelling, splash, muzzle flash
descent --robots-test        # the state machine, pathing, death and drops
descent --loop-test          # keys, doors, the reactor, the countdown, the exit
descent --world-test         # portal culling, light reach, shadow volume closure
descent --flight-test        # spin-up, coast-down, swipe-to-turn
descent --validate-assets    # every shipped model and level through the validators
```

Every run — test or game — ends with the achieved simulation step rate against
the 120 Hz target, the last frame's GPU cost per pass, and a count of heap
allocations made by the sim step and the draw path. All three should be boring.

`CLAUDE.md` lists the rest of the flags. `docs/MILESTONES.md` records what was
actually measured at each milestone, including the numbers and the bugs.

## Editing levels

`tools/editor/` is a browser tool for authoring `assets/levels/*.json`. It is
not part of the game build — no CMake target, no vcpkg entry, no node
toolchain.

```
tools/editor/serve.py        # prints the URL it bound to
```

It draws the mine with the game's own triangulation, checks every structural
rule the loader checks (and two it does not), and saves byte-exactly, so
opening a level and saving it is a no-op. **New** starts a mine from one room;
extrude grows it; **Verify** runs the real binary over the saved file and
reports what it said. See `tools/editor/README.md`.

```
node tools/editor/tests/conformance.mjs   # the editor's validator vs the game's
node tools/editor/tests/ops.mjs           # extrude, join, delete
node tools/editor/tests/entities.mjs      # lights, robots, pickups, doors
node tools/editor/tests/create.mjs        # starting a level from nothing
```

## Credits

- **Descent** (Parallax Software, 1995) for the segment-and-portal mine, the
  6DOF flight model, and the loop this recreates.
- **Doom 3** (id Software) for the stencil shadow volume implementation this
  follows, and for being the reference to read when something looks wrong.
  Depth-fail counting is Carmack's reverse.
- **F.E.A.R.** (Monolith) for the argument that hard shadows and no ambient term
  are a look worth committing to rather than a limitation to work around.

Dependencies, all from vcpkg and deliberately few: `glfw3`, `glad`, `glm`,
`imgui`, `stb`, `nlohmann-json`, `tinyobjloader`. The level editor is a
separate tool and not part of that build; it vendors three.js (MIT) in
`tools/editor/vendor/`.
