#!/usr/bin/env python3
"""Encode a HUD comm clip to the MPEG-1 the runtime decodes.

Requires ffmpeg on PATH. Like make-comm-frames.py, this is an authoring tool and
not a runtime or build dependency: what ships is the .mpg it writes.

The runtime decoder is pl_mpeg, which reads MPEG-1 video in an MPEG-PS
container and nothing else, so the encode is pinned to that rather than left to
ffmpeg's choice of defaults. Two details are not cosmetic:

  * Dimensions must be multiples of 16. MPEG-1 codes in 16x16 macroblocks, and
    every clip in a library shares one texture, so they must match each other
    too.
  * The GOP is short by default. Seeking exactly - which --comm does, and which
    a re-triggered message does - decodes forward from the preceding intra
    frame, so a long GOP makes every seek cost more.

With --pattern this needs no input file and writes a synthetic placeholder: a
test pattern with a burnt-in frame counter, so which frame is on screen can be
read off a screenshot.
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import subprocess
import wave
from pathlib import Path


def encode(source: Path | None, destination: Path, width: int, height: int, fps: float,
           seconds: float, bitrate: str, gop: int, pattern: bool) -> Path:
    if width % 16 or height % 16:
        raise SystemExit(f"{width}x{height} is not a multiple of 16; MPEG-1 codes in "
                         "16x16 macroblocks")
    destination.mkdir(parents=True, exist_ok=True)
    out = destination / "comm.mpg"

    if pattern:
        source_args = ["-f", "lavfi", "-i",
                       f"testsrc=size={width}x{height}:rate={fps}:duration={seconds}"]
    else:
        if source is None:
            raise SystemExit("an input file is required without --pattern")
        source_args = ["-i", str(source), "-t", str(seconds)]

    command = [
        "ffmpeg", "-y", "-hide_banner", "-loglevel", "error", *source_args,
        "-vf", (f"fps={fps},scale={width}:{height}:force_original_aspect_ratio=increase,"
                f"crop={width}:{height}"),
        "-c:v", "mpeg1video", "-b:v", bitrate, "-g", str(gop),
        # pl_mpeg decodes the audio track too if one is present. The game's own
        # mixer plays the transmission's sound, so the stream carries none.
        "-an", "-f", "mpeg", str(out),
    ]
    subprocess.run(command, check=True)
    return out


def write_tone(destination: Path, seconds: float, rate: int = 22050) -> Path:
    """A placeholder voice track: stdlib only, and the same bytes every run.

    Deliberately not speech. It is a slow two-tone chirp under an amplitude
    warble, so it reads as a comm carrier rather than as content anyone might
    mistake for a real line, and so the start and end are audible when checking
    that it lines up with the picture.

    The mixer resamples on load and downmixes to mono, so authoring at 22 kHz
    mono costs nothing at runtime and halves what the repository carries.
    """
    out = destination / "comm.wav"
    frames = int(seconds * rate)
    samples = bytearray()
    for i in range(frames):
        t = i / rate
        carrier = math.sin(2.0 * math.pi * (420.0 + 90.0 * math.sin(2.0 * math.pi * 0.35 * t)) * t)
        warble = 0.55 + 0.45 * math.sin(2.0 * math.pi * 6.0 * t)
        # Fade the ends so the clip neither clicks on nor cuts off.
        edge = min(1.0, t / 0.25, max(0.0, seconds - t) / 0.4)
        samples += struct.pack("<h", int(max(-1.0, min(1.0, carrier * warble * edge)) * 12000))
    with wave.open(str(out), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(rate)
        handle.writeframes(bytes(samples))
    return out


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("destination", type=Path, help="clip directory under assets/comm")
    parser.add_argument("source", type=Path, nargs="?", default=None)
    parser.add_argument("--pattern", action="store_true",
                        help="generate a synthetic placeholder instead of reading a source")
    parser.add_argument("--width", type=int, default=256)
    parser.add_argument("--height", type=int, default=192)
    parser.add_argument("--fps", type=float, default=24.0)
    parser.add_argument("--seconds", type=float, default=20.0)
    parser.add_argument("--bitrate", default="500k")
    parser.add_argument("--gop", type=int, default=12)
    parser.add_argument("--speaker", default="incoming transmission")
    parser.add_argument("--tone", action="store_true",
                        help="also write a placeholder comm.wav voice track")
    args = parser.parse_args()

    out = encode(args.source, args.destination, args.width, args.height, args.fps,
                 args.seconds, args.bitrate, args.gop, args.pattern)

    # displaySeconds is how long the panel stays up, which a Lua message may
    # override; the decoder loops to fill it if the video is shorter.
    manifest = {
        "video": out.name,
        "framesPerSecond": args.fps,
        "displaySeconds": args.seconds,
        "speaker": args.speaker,
    }
    (args.destination / "comm.json").write_text(json.dumps(manifest, indent=2) + "\n")
    size = out.stat().st_size
    note = ""
    if args.tone:
        wav = write_tone(args.destination, args.seconds)
        note = f", {wav.name} ({wav.stat().st_size / 1024:.0f} KiB)"
    print(f"{out} ({size / 1024:.0f} KiB){note} and comm.json written; "
          f"run 'descent --comm-test' to validate")


if __name__ == "__main__":
    main()
