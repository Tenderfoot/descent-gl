# Known issues

Things that are understood, reproducible, and deliberately not fixed yet. Each
entry says what is wrong, where, what it costs, and what to do instead in the
meantime. Something that is merely unbuilt belongs in `MILESTONES.md` under the
milestone that did not build it; this file is for behaviour that is already
shipped and already wrong.

---

## The panel's display duration still has more than one source of truth

**Where:** `assets/comm/<clip>/comm.json`, `assets/levels/*.lua`,
`src/render/comm_clip.cpp`.

**Partly fixed.** `tools/make-comm-video.py` no longer writes `framesPerSecond`
or `displaySeconds` for a decoded clip — the stream carries both and the loader
reads them from it — and `testcard`'s manifest has been cleaned, so the
committed disagreement (a manifest claiming 20.0s for a 19.833s video) is gone
and the tool can no longer create another. The tool also clamps `--seconds` to
the source's real length and writes the voice track at that same length, so a
clip it produces has its video and wav agree by construction.

What remains:

- A manifest *may* still declare `displaySeconds`, and should when the intent is
  to hold a clip on screen for longer than it runs. That is the field doing its
  job, but nothing distinguishes it from a stale copy of the stream's length.
- For a frame sequence the field is required and means something different —
  `intro` is 16 frames at 8 fps, 2.0s of pictures held for 4.5s by looping.
- A Lua message's `seconds` overrides whatever the clip says. This is ordinary
  layering rather than duplication: a clip cannot know that one use of it wants
  a short beat.
- Nothing cross-checks any of it. `--comm-test` does not compare a declared
  duration against the stream, or the voice track against either.

**To fix:** have `--comm-test` check the declared durations against the stream
and the wav, so an authoring mismatch fails headlessly instead of being noticed
by ear.

**Note:** `docs/ARCHITECTURE.md` claims, in the section on decoupling the comm
player from the simulation, that collapsing `GameConfig::commSeconds` made "the
three-clocks-must-agree problem stop existing". That was true when written and
is not true now — the Lua override arrived with the multiclip work and the voice
track arrived after it.

---

## A transmission's voice outlives its panel

**Where:** `src/main.cpp` — `stopComm()` is called only for the `--comm`
screenshot pin and on a level switch.

A message's `seconds` governs the picture and nothing else. Nothing stops the
voice when the panel retires, so a clip whose `comm.wav` is longer than the
message keeps talking with no panel on screen. With `testcard` on `lower_mine`
that would be 14.5 seconds of audio after the panel closed.

**Workaround:** author `seconds` to match the voice track's length. A clip built
by `make-comm-video.py` now has its video and wav the same length already, so
matching `seconds` to the video is enough.

**To fix:** make `seconds` authoritative for both, which is really the same
decision as the entry above — this is what "nobody owns the duration" looks like
in the mixer.

---

## Nothing triggers a decoded clip at runtime

**Where:** `assets/levels/mine01.lua`, `src/main.cpp` (`kDefaultCommClip`).

`testcard` is the only decoded clip and nothing reaches it. Both of `mine01`'s
Lua messages name `intro`, and nothing else raises a transmission at all now
that a level no longer opens with one. So in a running game:

- sequential decode never happens — `--comm testcard` pins one frame, so
  `CommVideo::frame` returns pixels once and null on every later draw;
- `TextureArray::updateLayer` runs exactly once per run, so the
  `glTextureSubImage3D` + `glGenerateTextureMipmap` cadence at 24 fps is never
  exercised;
- `Bank::loadComm` loads `testcard/comm.wav` and `Audio::playComm` is never
  reached with a real clip, because `intro` has no voice track. The committed
  wav is never heard by anything.

`--comm-test` covers the decoder hard (forward, seek, wrap, opacity,
allocations) but is headless by design and touches no GL.

**Workaround:** point a `mine01.lua` message's `clip` at `testcard` and run
`--segment 12`, which fires the trigger on the first simulation step. Revert
before release: it is shipped level content. This is now the only way to see a
transmission play, since `--comm` pins a single frame and nothing opens a level
with one.

**To fix:** either a flag that plays a named clip instead of pinning it, or a
test level that is not shipped content.

---

## The comm panel is described as much smaller than it is

**Where:** `src/render/comm_video.h`, `third_party/pl_mpeg/README.md`.

Both say the pictures land in "a panel about ninety pixels wide". They do not.
`panelW = min(w * 0.28, 92 * s)` where `s = max(1.5, height / 260)`, so 92 is a
base before a scale that is never below 1.5. Measured on a 1080p frame: 395 px
including the border, 382 px of image.

| Window | Panel image |
| --- | --- |
| 1280x720 | 255 px |
| 1920x1080 | 382 px |
| 2560x1440 | 510 px |
| 3840x2160 | 764 px |

This matters because it is the number anyone would use to pick a source
resolution, and it understates it by a factor of four at 1080p. A 256x192 source
is already being upscaled 1.49x at 1080p; 384x288 is very nearly pixel-native
there.

**To fix:** correct both comments. No code change.

---

## The draw path allocates

**Where:** pre-existing, not introduced by any recent milestone.

Every run reports `draw path SOME on 29 of 30 (worst 6)` after warm-up.
CLAUDE.md says both allocation lines should read `none` and that a number there
is a finding rather than noise. The sim-step line is clean; this one is not, and
has been identically non-zero on `main` across every branch measured against it.

Untraced. It has never been attributed to a specific call site.
