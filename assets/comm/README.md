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
