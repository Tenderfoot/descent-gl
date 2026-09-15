#!/usr/bin/env python3
"""Author a runtime-ready HUD comm sequence from a still image or video.

Video inputs require ffmpeg on PATH. Still images use Pillow and gain a small,
deterministic damaged-link animation so concept art can be previewed in-game.
"""

from __future__ import annotations

import argparse
import json
import random
import shutil
import subprocess
from pathlib import Path

from PIL import Image, ImageEnhance


VIDEO_SUFFIXES = {".mp4", ".mov", ".mkv", ".avi", ".webm"}


def fit_frame(image: Image.Image, width: int, height: int) -> Image.Image:
    scale = max(width / image.width, height / image.height)
    resized = image.resize((round(image.width * scale), round(image.height * scale)),
                           Image.Resampling.LANCZOS)
    left = (resized.width - width) // 2
    top = (resized.height - height) // 2
    return resized.crop((left, top, left + width, top + height)).convert("RGB")


def still_frames(source: Path, destination: Path, width: int, height: int,
                 frame_count: int) -> list[str]:
    base = fit_frame(Image.open(source), width, height)
    names: list[str] = []
    for index in range(frame_count):
        rng = random.Random(0xD35CE17 + index)
        frame = ImageEnhance.Brightness(base).enhance(0.96 + rng.random() * 0.08)

        # Two thin horizontal tears move independently. Pixels wrap rather
        # than disappear, which reads as a bad transmission instead of damage
        # to the authored portrait.
        for tear in range(2):
            band_y = (index * (17 + tear * 6) + tear * 61) % height
            band_h = 2 + tear
            shift = (-1 if (index + tear) % 2 else 1) * (2 + index % 4)
            strip = frame.crop((0, band_y, width, min(height, band_y + band_h)))
            wrapped = Image.new("RGB", strip.size)
            wrapped.paste(strip, (shift, 0))
            wrapped.paste(strip, (shift - width if shift > 0 else shift + width, 0))
            frame.paste(wrapped, (0, band_y))

        name = f"frame_{index:03d}.png"
        frame.save(destination / name, optimize=True)
        names.append(name)
    return names


def video_frames(source: Path, destination: Path, width: int, height: int,
                 fps: float) -> list[str]:
    if not shutil.which("ffmpeg"):
        raise SystemExit("video input requires ffmpeg on PATH")
    vf = (f"fps={fps},scale={width}:{height}:force_original_aspect_ratio=increase,"
          f"crop={width}:{height}")
    subprocess.run([
        "ffmpeg", "-y", "-i", str(source), "-an", "-vf", vf,
        str(destination / "frame_%03d.png"),
    ], check=True)
    return [path.name for path in sorted(destination.glob("frame_*.png"))]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--width", type=int, default=256)
    parser.add_argument("--height", type=int, default=192)
    parser.add_argument("--fps", type=float, default=8.0)
    parser.add_argument("--seconds", type=float, default=4.5,
                        help="time the looping clip remains on the HUD")
    parser.add_argument("--frames", type=int, default=16,
                        help="frames generated for a still input")
    args = parser.parse_args()

    if args.width <= 0 or args.height <= 0 or args.fps <= 0 or args.seconds <= 0:
        parser.error("dimensions, fps and seconds must be positive")
    args.destination.mkdir(parents=True, exist_ok=True)
    for old in args.destination.glob("frame_*.png"):
        old.unlink()

    if args.source.suffix.lower() in VIDEO_SUFFIXES:
        frames = video_frames(args.source, args.destination, args.width, args.height, args.fps)
    else:
        frames = still_frames(args.source, args.destination, args.width, args.height, args.frames)
    if not frames:
        raise SystemExit("source produced no frames")

    manifest = {
        "framesPerSecond": args.fps,
        "displaySeconds": args.seconds,
        "frames": frames,
    }
    (args.destination / "comm.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                                  encoding="utf-8")
    print(f"wrote {len(frames)} frames and {args.destination / 'comm.json'}")


if __name__ == "__main__":
    main()
