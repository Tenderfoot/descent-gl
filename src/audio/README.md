# src/audio

Sound. Loads WAV files, mixes them, positions them in the mine, and pushes the
result at a playback device.

The rule here is the mirror of `src/sim`'s: **nothing under `src/sim` includes
anything from here.** The simulation reports what happened into a
`sim::EventQueue` (see `sim/events.h`) and this is the only thing that decides
that a laser firing is `laser.wav`, that it should be quieter than a missile,
and that it is forty units behind you.

| File | What it is |
| --- | --- |
| `wav.h/.cpp` | A RIFF/WAVE reader. PCM 8/16/24/32 and float, any rate, any channel count, downmixed to mono and resampled to `kSampleRate`. |
| `mixer.h/.cpp` | The voice pool, distance attenuation, equal-power panning and the soft-clipped bus. Pure DSP over plain arrays — no device, no library, no game. |
| `audio.h/.cpp` | The bank (which clip each cue uses, and how loud) and the façade the game talks to. |
| `device.h/.cpp` | miniaudio. The **only** file in the project that includes it. |
| `sound_test.h/.cpp` | `--sound-test`: everything above, with no device and no sound card. |

The split exists so the interesting half can be tested. `Mixer` has no
dependency on miniaudio, on GLFW or on a working audio device, which is why
`--sound-test` can drive attenuation curves, the stereo image, voice stealing
and the loop path in a headless container and report numbers.

## The thread

`Mixer::render` runs on a thread owned by the audio driver, under a hard
deadline. Return late and the driver replays a stale buffer, which is an audible
click. So the render path **allocates nothing, takes no lock, and blocks on
nothing**. The game thread talks to it only by pushing POD commands into a
single-producer/single-consumer ring (`Ring` in `mixer.h`); the audio thread
drains that ring at the top of every callback. `--sound-test` checks the
no-allocation half with the same counter `main.cpp` uses on the sim step.

## Adding a sound

1. Put the WAV in `assets/sounds/`, or add it to `tools/make-sounds.py` and
   regenerate. Any rate, any bit depth, mono or stereo — `wav.cpp` copes.
2. Add a `Cue` to `src/sim/events.h`, and its name to `kCueNames`.
3. Add the matching row to `kCues` in `audio.cpp` — filename, gain, detune.
   The table is in enum order and the compiler will not tell you if it is not,
   so count.
4. Emit it from wherever in `src/sim` it happens, with `emit()` or
   `emitLocal()`.
5. `--sound-test` will fail until step 1 and step 3 agree.
