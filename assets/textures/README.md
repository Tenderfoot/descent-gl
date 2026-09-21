# Wall tiles

The mine's surfaces. Every tile here is one layer of a single texture array, so
the whole mine draws with one binding and a level file's `texture: N` picks a
layer by its position in `tiles.json`.

These used to be generated in C++ at startup, which made a new tile a code
change and a rebuild. They are ordinary PNGs now, with the same standing as the
WAVs in `assets/sounds/`: **the manifest is the contract, not the contents.**
Repaint `rock.png` and the mine changes; nothing in `src/` needs to know.

## Adding a tile

Drop the PNG in, then append a line to `tiles.json`:

```json
{ "name": "bulkhead" }
```

Append rather than insert. A tile's position *is* its layer index, so putting
one in the middle renumbers every tile after it and silently re-textures every
level already authored against the old order. `file` is optional and defaults to
`<name>.png`, for pointing a name at art whose filename you would rather not
change.

The editor reads the same manifest — `/api/textures` in `tools/editor/serve.py`
— so a tile added here appears in the side panel's picker with no other change.

## What a tile has to be

- **Square, and at least the manifest's `size`.** Every layer of a texture
  array shares one size; larger art is resampled down at load, smaller art is a
  fatal error. Raising `size` raises it for the whole set.
- **Bright.** There is no ambient term anywhere in the renderer, so albedo is
  the only thing keeping a lit surface off pure black. A moody dark cave texture
  does not read as gloomy, it reads as nothing. The shipped set sits between 105
  and 156 mean luminance.
- **Tiling.** UVs are planar at 5.5 world units per tile (`kWorldUnitsPerTile`)
  against 20-unit segments, so a wall shows about four repeats and a seam is
  obvious. Hard edges at the tile boundary are fine — a plate groove is supposed
  to look like an edge — but a smooth tile that jumps where it joins itself is a
  line across the wall.

`descent --validate-assets` measures the last two and warns rather than failing,
since a tile may be dark or hard-edged on purpose.

## Four names the renderer needs

Most tiles are the level author's to arrange, but four are chosen by the
renderer rather than by a level, and the game will not start without them:

| name | used for |
| --- | --- |
| `rock` | `pillar` props |
| `panel` | robots, and every prop that is not a pillar |
| `hazard` | door slabs |
| `hull` | the ship |

## Regenerating the shipped set

`tools/make-textures.py` writes the five tiles that shipped, so the art is
reproducible rather than opaque binaries nobody can rebuild:

```
tools/make-textures.py                 # rewrite every generated tile
tools/make-textures.py --only rock     # rewrite one, leave the rest alone
```

It is committed, deterministic and stdlib-only. There is deliberately no
`--check` mode: a tile you have repainted is the feature working, not a failure.
Procedural generation is still the better tool for the regular geometric ones —
the hazard chevrons, the grate, the plate-and-rivet patterns — and the worse
tool for anything organic.
