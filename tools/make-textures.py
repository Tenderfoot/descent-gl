#!/usr/bin/env python3
"""Write assets/textures/*.png.

The game loads the wall tiles from assets/textures/ at runtime and does not
care where they came from; this is where the shipped set comes from. It is
committed, deterministic and stdlib-only, so the assets can be regenerated and
diffed rather than being opaque binaries nobody can reproduce:

    tools/make-textures.py                  # rewrite every generated tile
    tools/make-textures.py --only rock      # rewrite one, leave the rest alone

It writes at the size assets/textures/tiles.json declares, so a regenerated
tile always matches the set. Every generator is written against the tile rather
than against a pixel count, so --size changes the resolution and not the
picture.

Any of these files may be replaced with painted or downloaded art;
src/render/wall_tiles.cpp loads whatever PNG the manifest names and resamples
it, so nothing has to match what is written here. The manifest is the
contract - assets/textures/tiles.json - not the contents. That is also why
there is no --check mode: a tile you have repainted is the feature working,
not a failure.

Two rules a replacement tile has to keep, both of them checked by
`descent --validate-assets`:

  * It has to be bright. There is no ambient term anywhere in the renderer, so
    albedo is the only thing keeping a lit surface off pure black; a moody dark
    cave texture renders as black. See the note above makeRock below.
  * It has to tile. UVs are planar at 5.5 world units per tile against 20-unit
    segments, so a wall shows about four repeats and a seam is obvious.

## Why the float rounding is spelled out

These tiles used to be generated in C++ at startup, in `float`. This script is
a port of that code, and it rounds every intermediate to single precision -
that is what `f32()` is for - so the PNGs it writes are the tiles the game
already drew rather than something a hair different. The literals that are not
exactly representable in single precision (0.6, 0.3, 0.1, 0.94, 0.86, 0.09) are
rounded once, up front, for the same reason.

Once you repaint a tile none of that matters for it any more. It matters until
then, so that moving to files changes nothing you can see.
"""

import argparse
import json
import math
import struct
import sys
import zlib
from functools import lru_cache
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEXTURES = ROOT / "assets" / "textures"

# Fallback if the manifest cannot be read. The size that matters is the one in
# tiles.json, because a tile smaller than the set the manifest declares is a
# fatal error at load.
FALLBACK_SIZE = 256


def manifest_size():
    try:
        with open(TEXTURES / "tiles.json", encoding="utf-8") as f:
            return int(json.load(f)["size"])
    except (OSError, ValueError, KeyError, TypeError):
        return FALLBACK_SIZE

# ------------------------------------------------------------ single precision

_F32 = struct.Struct("f")


def f32(x):
    """Round a Python float to the nearest single-precision value."""
    return _F32.unpack(_F32.pack(x))[0]


C_0_6, C_0_3, C_0_1 = f32(0.6), f32(0.3), f32(0.1)
C_0_94, C_0_86 = f32(0.94), f32(0.86)
C_0_09 = f32(0.09)

# ------------------------------------------------------------------- the noise

# Deterministic value noise. Not a good noise function; good enough to break up
# a flat colour, and identical from run to run so a screenshot can be compared
# against an earlier one.
#
# Memoised because every caller samples it on a small integer lattice - a tile
# at SIZE=256 touches at most a few hundred distinct points per octave - so the
# cache turns the inner loop from hashing into dictionary lookups.


@lru_cache(maxsize=None)
def hash_noise(x, y, seed):
    h = (x * 374761393 + y * 668265263 + seed * 2246822519) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return f32(((h ^ (h >> 16)) & 0xFFFF) / 65535.0)


def smooth_noise(x, y, seed):
    xi = math.floor(x)
    yi = math.floor(y)
    fx = f32(x - xi)
    fy = f32(y - yi)
    sx = f32(f32(fx * fx) * f32(3.0 - f32(2.0 * fx)))
    sy = f32(f32(fy * fy) * f32(3.0 - f32(2.0 * fy)))

    a = hash_noise(xi, yi, seed)
    b = hash_noise(xi + 1, yi, seed)
    c = hash_noise(xi, yi + 1, seed)
    d = hash_noise(xi + 1, yi + 1, seed)

    ab = f32(a + f32(f32(b - a) * sx))
    cd = f32(c + f32(f32(d - c) * sx))
    return f32(ab + f32(f32(cd - ab) * sy))


def clamp8(v):
    return 0 if v < 0 else (255 if v > 255 else v)


class Tile:
    """RGBA pixels, row 0 at the BOTTOM of the wall.

    v = 0 is the bottom corner of a side (src/game/level.cpp lays the UVs out
    that way), and a GL texture's first row is v = 0. write_png flips on the
    way out so the file is the right way up in an image editor, and
    wall_tiles.cpp flips it back on the way in.
    """

    def __init__(self, size):
        self.size = size
        self.px = bytearray(size * size * 4)

    def put(self, x, y, r, g, b, a=255):
        i = (y * self.size + x) * 4
        self.px[i + 0] = clamp8(r)
        self.px[i + 1] = clamp8(g)
        self.px[i + 2] = clamp8(b)
        self.px[i + 3] = clamp8(a)

# --------------------------------------------------------------- the generators

# Tiles are authored bright. With no ambient term the only thing that darkens a
# surface is distance from a light, so an albedo that already looks "dark and
# cave-like" would render as pure black almost everywhere.


def make_rock(size):
    tile = Tile(size)
    scale = f32(8.0 / size)
    for y in range(size):
        for x in range(size):
            fx = f32(x * scale)
            fy = f32(y * scale)
            n = f32(
                f32(
                    f32(smooth_noise(fx, fy, 11) * C_0_6)
                    + f32(smooth_noise(f32(fx * 3.0), f32(fy * 3.0), 12) * C_0_3)
                )
                + f32(smooth_noise(f32(fx * 9.0), f32(fy * 9.0), 13) * C_0_1)
            )
            v = int(f32(120.0 + f32(n * 90.0)))
            tile.put(x, y, v, int(f32(v * C_0_94)), int(f32(v * C_0_86)))
    return tile


def make_panel(size):
    tile = Tile(size)
    plate = max(1, size // 4)
    seam = max(1, size // 64)
    scale = f32(16.0 / size)
    rr = max(2, size // 64)
    cx = cy = plate // 8
    for y in range(size):
        for x in range(size):
            pxi = x // plate
            pyi = y // plate
            jitter = int(f32(hash_noise(pxi, pyi, 21) * 26.0)) - 13
            grain = f32(smooth_noise(f32(x * scale), f32(y * scale), 22) * 18.0)
            v = 150 + jitter + int(grain)
            r, g, b = v, v + 4, v + 14
            if x % plate < seam or y % plate < seam:
                r, g, b = 78, 84, 96
            # Rivets at the plate corners.
            rx, ry = x % plate, y % plate
            if (rx - cx) * (rx - cx) + (ry - cy) * (ry - cy) < rr * rr:
                r, g, b = 196, 200, 210
            tile.put(x, y, r, g, b)
    return tile


def make_hazard(size):
    tile = Tile(size)
    stripe = max(1, size // 8)
    # The wear noise is the one term in this file that was written against a
    # pixel count rather than against the tile, so at 1024 it would come out
    # four times finer than the tile it was tuned for. Dividing by the size it
    # was written at makes --size mean "the same tile, more pixels" here as it
    # does everywhere else; at 256 the factor is exactly 1 and nothing moves.
    grime = f32(C_0_09 * f32(256.0 / size))
    for y in range(size):
        for x in range(size):
            # Diagonal bands, wrapping so the tile still repeats seamlessly.
            band = ((x + y) % size) // stripe
            warn = band % 2 == 0
            n = smooth_noise(f32(x * grime), f32(y * grime), 31)
            wear = int(f32(n * 34.0)) - 17
            if warn:
                tile.put(x, y, 224 + wear, 168 + wear, 38 + wear)
            else:
                tile.put(x, y, 54 + wear, 52 + wear, 56 + wear)
    return tile


def make_grate(size):
    tile = Tile(size)
    cell = max(2, size // 10)
    bar = max(2, cell // 3)
    for y in range(size):
        for x in range(size):
            if (x % cell) < bar or (y % cell) < bar:
                # Shade the bar across its width so it reads as round.
                across = min(x % cell, y % cell)
                shade = 130 + (across * 70) // max(1, bar)
                tile.put(x, y, shade, shade + 4, shade + 12)
            else:
                tile.put(x, y, 34, 36, 42)
    return tile


def make_grate_open(size):
    """A grate with real holes, for Level::grates: the gaps are alpha 0, so the
    walls show through them and a light throws the bars as a shadow.

    Coarser than `grate`, so the bars still read when their shadow is thrown
    across a room. The gaps carry the bars' base colour rather than black:
    mipmapping and bilinear filtering average RGB across the cut-out's edge,
    and a black gap would put a dark rim on every bar."""
    tile = Tile(size)
    cell = max(4, size // 4)
    bar = max(2, cell // 4)
    for y in range(size):
        for x in range(size):
            if (x % cell) < bar or (y % cell) < bar:
                across = min(x % cell, y % cell)
                shade = 130 + (across * 70) // max(1, bar)
                tile.put(x, y, shade, shade + 4, shade + 12)
            else:
                tile.put(x, y, 130, 134, 142, 0)
    return tile


def make_hull(size):
    """Panelled metal for the ship hull: plate seams, a little per-plate
    variation, and a warm stripe so roll is readable from the chase camera."""
    tile = Tile(size)
    plate = max(1, size // 4)
    seam = max(1, size // 96)
    band = size // 2
    band_height = max(2, size // 42)
    for y in range(size):
        for x in range(size):
            # Cheap hash per plate so neighbouring plates differ slightly and
            # the hull does not read as one flat colour.
            px = x // plate
            py = y // plate
            h = ((px * 73856093) ^ (py * 19349663)) & 0xFFFFFFFF
            jitter = (h % 17) - 8

            r, g, b = 104 + jitter, 110 + jitter, 122 + jitter
            if x % plate < seam or y % plate < seam:
                r, g, b = 52, 56, 64
            # One warm stripe across the tile.
            if band <= y < band + band_height:
                r, g, b = 198, 126, 44
            tile.put(x, y, r, g, b)
    return tile


GENERATORS = {
    "rock": make_rock,
    "panel": make_panel,
    "hazard": make_hazard,
    "grate": make_grate,
    "hull": make_hull,
    "grate-open": make_grate_open,
}

# --------------------------------------------------------------------- the PNG


def write_png(path, tile):
    """An 8-bit RGBA PNG, rows flipped so the file is the right way up.

    Filter type 0 on every scanline: these are small and the point is a file
    anyone can regenerate, not the last byte of compression.
    """
    size = tile.size
    stride = size * 4
    raw = bytearray()
    for y in range(size - 1, -1, -1):
        raw.append(0)
        raw += tile.px[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    path.write_bytes(png)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--only",
        help="comma-separated tile names to rewrite; the default is all of them",
    )
    parser.add_argument(
        "--size",
        type=int,
        default=None,
        help="pixels square; the default is the manifest's \"size\"",
    )
    args = parser.parse_args(argv)
    if args.size is None:
        args.size = manifest_size()
    if args.size < 1:
        parser.error("--size must be positive")

    names = list(GENERATORS)
    if args.only:
        names = [n.strip() for n in args.only.split(",") if n.strip()]
        unknown = [n for n in names if n not in GENERATORS]
        if unknown:
            parser.error(f"no generator for {', '.join(unknown)}; have {', '.join(GENERATORS)}")

    TEXTURES.mkdir(parents=True, exist_ok=True)
    for name in names:
        tile = GENERATORS[name](args.size)
        path = TEXTURES / f"{name}.png"
        write_png(path, tile)
        print(f"{path.relative_to(ROOT)}  {args.size}x{args.size}  {path.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
