# Known issues

Things that are understood, reproducible, and deliberately not fixed yet. Each
entry says what is wrong, where, what it costs, and what to do instead in the
meantime. Something that is merely unbuilt belongs in `MILESTONES.md` under the
milestone that did not build it; this file is for behaviour that is already
shipped and already wrong.

---

## The panel's display duration has four sources of truth

**Where:** `assets/comm/<clip>/comm.json`, `assets/levels/*.lua`,
`src/render/comm_clip.cpp`, `src/main.cpp`.

How long a comm panel stays on screen is declared in four places, and nothing
cross-checks them. On `testcard` they already disagree:

| Source | Says |
| --- | --- |
| `comm.json` `displaySeconds` | 20.0 s |
| the video's own length | **19.833 s** (476 frames at 24 fps) |
| `comm.wav` | 20.0 s |
| `mine01.lua` `seconds` | 5.5 s |

The loader prefers the manifest over the file, so for a decoded clip the
manifest's number wins even when the stream disagrees with it.

Not all four are equally wrong. A message's `seconds` overriding a clip default
is ordinary layering, and a clip cannot know that one use of it wants a 5.5
second beat. The duplication is `displaySeconds` in a *video's* manifest, which
restates what the file already knows — `loadCommClips` treats it as optional and
falls back to the decoder, but `tools/make-comm-video.py` writes it anyway, which
is how the 0.17 s disagreement above got committed. For a frame sequence the
field does real work: `intro` is 16 frames at 8 fps, 2.0 s of pictures held for
4.5 s by looping. So it means two different things depending on the clip kind.

**Workaround:** put the video's real length in the message's `seconds`, which is
the value that actually governs playback.

**To fix:** stop the authoring tool writing `displaySeconds` for a video, and
have `--comm-test` check the declared durations against the stream and against
the voice track so an authoring mismatch fails headlessly.

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

**Workaround:** author `seconds` to match the voice track's length.

**To fix:** make `seconds` authoritative for both, which is really the same
decision as the entry above — this is what "nobody owns the duration" looks like
in the mixer.

---

## Nothing triggers a decoded clip at runtime

**Where:** `assets/levels/mine01.lua`, `src/main.cpp` (`kDefaultCommClip`).

`testcard` is the only decoded clip and nothing reaches it. The default
transmission is `intro`, and both Lua messages name `intro`. So in a running
game:

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
before release: it is shipped level content.

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
