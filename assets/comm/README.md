# HUD communications

The runtime does not decode video. Each communication is a JSON manifest and
a set of equal-sized RGBA PNG frames, uploaded once into a texture array.

`intro/comm.json` is the level-opening transmission. It loops its authored
frames for `displaySeconds`; simulation time selects the frame, so fixed-step
tests and screenshots remain deterministic.

To author or replace a sequence:

```powershell
python tools/make-comm-frames.py input.mp4 assets/comm/intro `
  --width 256 --height 192 --fps 8 --seconds 4.5
```

Video input uses `ffmpeg` for offline extraction. A still image uses Pillow to
make a short damaged-link loop. Neither tool is part of the game or its runtime
dependencies. Run `descent --comm-test` after changing a sequence.

## Decoded clips

A clip directory holds either authored frames or a video, and the library loads
both so a set can be converted one clip at a time. A manifest naming `video` is
decoded; one listing `frames` is uploaded.

```json
{ "video": "comm.mpg", "framesPerSecond": 24.0, "displaySeconds": 20.0 }
```

`framesPerSecond` and `displaySeconds` are optional for a video — the stream
knows both — and a Lua message overrides how long the panel stays up regardless,
looping the pictures to fill it.

The runtime decoder is pl_mpeg, so the file must be MPEG-1 video in an MPEG-PS
container, with dimensions a multiple of 16 that match every other clip in the
library. `tools/make-comm-video.py` encodes that:

```
python tools/make-comm-video.py assets/comm/intro input.mp4 --seconds 20
```

Why bother: a frame sequence costs width*height*4 bytes of texture for every
frame, resident from startup, so a library's video memory and the repository
both grow with the total authored seconds across all of it. `testcard` is 20
seconds at 24 fps — 480 frames, which as PNGs would be roughly 39 MB and about
126 MB of texture with mipmaps. Decoded it is 544 KiB on disk and one texture
layer, and a longer clip costs neither more.

`testcard` itself is a synthetic placeholder, not content: a test pattern with a
burnt-in frame counter, so which frame is on screen can be read off a
screenshot. `--pattern` regenerates it and `--tone` writes its placeholder voice
track.

## Voice tracks

A `comm.wav` beside the pictures is the transmission's voice. It is optional —
a clip without one plays silently — and it is loaded into the ordinary sound
bank, then played on the `Comm` ambient channel, which means a firefight cannot
steal it mid-sentence and it ends with its own recording rather than looping.

Picture and voice are started together and never corrected against each other
afterwards. See `docs/ARCHITECTURE.md`.
