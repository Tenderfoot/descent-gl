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

The video carries no audio stream, because pl_mpeg would decode one and the
game plays a transmission's voice through its own mixer instead. The soundtrack
is not thrown away though: it is written beside the video as comm.wav, trimmed
to the same length, which is where the engine looks for it. --no-audio skips
that, and --tone writes a synthetic one for a source that has none.

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


def probe(source: Path, entries: str) -> str:
    """One ffprobe field, or empty if ffprobe cannot answer."""
    try:
        done = subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries", entries, "-of", "csv=p=0", str(source)],
            check=True, capture_output=True, text=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        return ""
    return done.stdout.strip()


def source_seconds(source: Path, requested: float) -> float:
    """`requested`, but never longer than the source actually is.

    Asking for more than the file holds does not make a longer video - ffmpeg
    stops at the end - it just makes the manifest claim a duration the stream
    does not have. Clamping here is the cheapest place to keep the two honest.
    """
    raw = probe(source, "format=duration")
    try:
        actual = float(raw)
    except ValueError:
        return requested
    if actual > 0.0 and actual < requested:
        print(f"note: {source.name} is {actual:.2f}s, shorter than the {requested:.2f}s asked "
              f"for; using {actual:.2f}s")
        return actual
    return requested


def has_audio(source: Path) -> bool:
    return "audio" in probe(source, "stream=codec_type").split()


def extract_audio(source: Path, destination: Path, seconds: float, rate: int = 22050) -> Path:
    """The source's own soundtrack, as the voice track the engine expects.

    The video deliberately carries no audio stream, so without this the sound
    is simply dropped. Mono because audio::Clip is mono and a stereo file is
    downmixed on load anyway; 22 kHz because everything is resampled to 44.1 on
    load, so authoring higher costs the repository size and buys nothing.

    Trimmed to the same `seconds` as the video, so picture and voice are the
    same length by construction rather than by someone remembering.
    """
    out = destination / "comm.wav"
    subprocess.run(
        ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-i", str(source),
         "-t", str(seconds), "-vn", "-ac", "1", "-ar", str(rate), "-c:a", "pcm_s16le", str(out)],
        check=True)
    return out


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
                        help="write a synthetic placeholder comm.wav (for --pattern, which has "
                             "no soundtrack to take)")
    parser.add_argument("--no-audio", action="store_true",
                        help="discard the source's soundtrack instead of writing comm.wav")
    args = parser.parse_args()

    seconds = args.seconds
    if args.source is not None and not args.pattern:
        seconds = source_seconds(args.source, seconds)

    out = encode(args.source, args.destination, args.width, args.height, args.fps,
                 seconds, args.bitrate, args.gop, args.pattern)

    # Neither framesPerSecond nor displaySeconds is written. The stream carries
    # both and the loader reads them from it; restating them here only creates a
    # second number that can disagree with the file, which is exactly how
    # testcard ended up declaring 20.0s for a 19.833s video. Add displaySeconds
    # by hand only to hold a clip on screen for longer than it runs, or say so
    # in the Lua message, which overrides either way.
    manifest = {"video": out.name, "speaker": args.speaker}
    (args.destination / "comm.json").write_text(json.dumps(manifest, indent=2) + "\n")

    note = ""
    if args.tone:
        wav = write_tone(args.destination, seconds)
        note = f", {wav.name} ({wav.stat().st_size / 1024:.0f} KiB, synthetic)"
    elif args.no_audio:
        note = ", soundtrack discarded (--no-audio)"
    elif args.source is not None and has_audio(args.source):
        wav = extract_audio(args.source, args.destination, seconds)
        note = f", {wav.name} ({wav.stat().st_size / 1024:.0f} KiB, from the source)"
    elif args.source is not None:
        note = ", no soundtrack in the source"

    print(f"{out} ({out.stat().st_size / 1024:.0f} KiB){note} and comm.json written; "
          f"run 'descent --comm-test' to validate")


if __name__ == "__main__":
    main()
