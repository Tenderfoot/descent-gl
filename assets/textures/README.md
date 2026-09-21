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
  does not read as gloomy, it reads as nothing. The shipped set sits between 63
  and 156 mean luminance, the painted tiles at the darker end.
- **Tiling.** UVs are planar at 5.5 world units per tile (`kWorldUnitsPerTile`)
  against 20-unit segments, so a wall shows about four repeats and a seam is
  obvious. Hard edges at the tile boundary are fine — a plate groove is supposed
  to look like an edge — but a smooth tile that jumps where it joins itself is a
  line across the wall.

`descent --validate-assets` measures the last two and warns rather than failing,
since a tile may be dark or hard-edged on purpose.

As it stands the ten painted tiles all wrap exactly, and two of the generated
ones do not: `rock` reports a seam 42x its own interior variation and `hazard`
8.5x. The value noise behind them is not periodic, so it does not meet itself
where the tile repeats. It got worse rather than better when the set moved to
1024, because the noise is smoother per pixel at higher resolution while the
join is just as hard. Worth knowing before reaching for `rock`, which is also
the tile every unpainted wall defaults to.

## Emitting, and moving

A wall is normally only ever as bright as the lights that reach it. A side with
`emissive` set adds its own albedo on top of that, which is what a lava pool or
a reactor glow is:

```json
{ "texture": 13, "emissive": 2.5 }
```

The editor's side panel has an `emits` box for it. This is emissive, not
ambient — the rest of the mine stays exactly as black as it was. It also lights
nothing: the renderer has no area lights, so a pool that should lay light on the
room around it wants a `Light` in the segment as well, the same pairing as a
lamp and its fixture. The editor offers to drop one for you.

A tile can also move, and the manifest is where that lives:

```json
{ "name": "lava", "scroll": [0.0, 0.03], "turbulence": 0.02 }
```

`scroll` slides the lookup in UV units per second. `turbulence` is the amplitude
of a sine warp of it — the old flowing-lava trick — whose period is exactly one
tile, so animating cannot pull a seam into a texture that tiles.

Both are applied **only in the emissive pass**, so they show up on sides that
emit and nowhere else. That is deliberate: the lit shader runs once per light
per fragment, so anything animated there is computed again for every light in
the room, while the emissive pass runs once. A tile bright enough to be worth
animating is being drawn by that pass anyway.

`reactor-plate` ships with a slow turbulence, so setting `emits` on a wall that
uses it is the shortest way to see this working. Delete the field to still it.
Animation is phased off the level's own elapsed time from the fixed-step
simulation, not a wall clock, so `--frames N --screenshot` still renders the
same picture every run.

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

`tools/make-textures.py` writes the five generated tiles - `rock`, `panel`,
`hazard`, `grate` and `hull` - so that part of the set is reproducible rather
than opaque binaries nobody can rebuild. The other ten are painted and this
script knows nothing about them:

```
tools/make-textures.py                 # rewrite every generated tile
tools/make-textures.py --only rock     # rewrite one, leave the rest alone
```

It writes at the size `tiles.json` declares, so a regenerated tile always
matches the set. Every generator is written against the tile rather than against
a pixel count, so `--size` changes the resolution and not the picture.

It is committed, deterministic and stdlib-only. There is deliberately no
`--check` mode: a tile you have repainted is the feature working, not a failure.
Procedural generation is still the better tool for the regular geometric ones —
the hazard chevrons, the grate, the plate-and-rivet patterns — and the worse
tool for anything organic.
