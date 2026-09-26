#!/usr/bin/env python3
"""Write assets/sounds/*.wav.

The game loads WAV files at runtime and does not care where they came from;
this is where the shipped set comes from. It is committed, deterministic and
stdlib-only, so the assets can be regenerated and diffed rather than being
opaque binaries nobody can reproduce:

    tools/make-sounds.py            # rewrite assets/sounds/
    tools/make-sounds.py --check    # fail if the committed files differ

Any of these files may be replaced with a recording or a downloaded effect;
src/audio/wav.cpp reads 8/16/24/32-bit PCM and float WAV at any rate and any
channel count, so nothing has to match what is written here. The names are the
contract - see kCues in src/audio/audio.cpp - not the contents.

Everything is mono 16-bit PCM at 44.1 kHz, which is what the mixer resamples to
anyway. engine.wav and alarm.wav loop, so they are built to be seamless: see
seamless() below.
"""

import argparse
import math
import os
import random
import struct
import sys

RATE = 44100

# ---------------------------------------------------------------- generators


def frames(seconds):
    return int(RATE * seconds)


def silence(seconds):
    return [0.0] * frames(seconds)


def sweep(seconds, f0, f1, shape="sin", curve="exp", phase=0.0):
    """A tone whose frequency slides from f0 to f1."""
    n = frames(seconds)
    out = [0.0] * n
    p = phase
    for i in range(n):
        t = i / max(1, n - 1)
        f = f0 * (f1 / f0) ** t if curve == "exp" and f0 > 0 and f1 > 0 else f0 + (f1 - f0) * t
        p += 2.0 * math.pi * f / RATE
        if shape == "sin":
            out[i] = math.sin(p)
        elif shape == "square":
            out[i] = 1.0 if math.sin(p) >= 0.0 else -1.0
        elif shape == "saw":
            out[i] = 2.0 * ((p / (2.0 * math.pi)) % 1.0) - 1.0
        elif shape == "tri":
            x = (p / (2.0 * math.pi)) % 1.0
            out[i] = 4.0 * abs(x - 0.5) - 1.0
    return out


def tone(seconds, freq, shape="sin", phase=0.0):
    return sweep(seconds, freq, freq, shape=shape, curve="lin", phase=phase)


def noise(seconds, rng):
    return [rng.uniform(-1.0, 1.0) for _ in range(frames(seconds))]


# ------------------------------------------------------------------- shaping


def env(buf, attack=0.002, decay=0.0, sustain=1.0, release=0.05, power=1.0):
    """An ADSR over the whole buffer, in seconds. Release is measured from the end."""
    n = len(buf)
    a = max(1, frames(attack))
    d = frames(decay)
    r = max(1, frames(release))
    out = [0.0] * n
    for i in range(n):
        if i < a:
            g = i / a
        elif d and i < a + d:
            g = 1.0 - (1.0 - sustain) * ((i - a) / d)
        elif i >= n - r:
            g = sustain * max(0.0, (n - i) / r)
        else:
            g = sustain
        out[i] = buf[i] * (g ** power)
    return out


def decay_env(buf, tau, power=1.0):
    """Exponential decay with time constant `tau` seconds - a hit, not a note."""
    return [
        v * (math.exp(-(i / RATE) / tau) ** power) for i, v in enumerate(buf)
    ]


def lowpass(buf, cutoff):
    """One-pole. Not a filter design, just enough to take the fizz off noise."""
    if cutoff <= 0:
        return list(buf)
    alpha = 1.0 - math.exp(-2.0 * math.pi * cutoff / RATE)
    out = [0.0] * len(buf)
    y = 0.0
    for i, v in enumerate(buf):
        y += alpha * (v - y)
        out[i] = y
    return out


def highpass(buf, cutoff):
    lo = lowpass(buf, cutoff)
    return [v - l for v, l in zip(buf, lo)]


def mix(*buffers):
    n = max(len(b) for b in buffers)
    out = [0.0] * n
    for b in buffers:
        for i, v in enumerate(b):
            out[i] += v
    return out


def gain(buf, g):
    return [v * g for v in buf]


def concat(*buffers):
    out = []
    for b in buffers:
        out.extend(b)
    return out


def normalise(buf, peak=0.89):
    """Leave headroom. Several of these play at once and the mixer sums them."""
    high = max((abs(v) for v in buf), default=0.0)
    if high < 1e-9:
        return buf
    return [v * (peak / high) for v in buf]


def seamless(buf, fade=0.06):
    """Crossfade the tail over the head so a loop has no seam.

    The caller generates `fade` seconds more than the loop should be; that extra
    tail is faded in over the start while the start fades out under it. The
    result is exactly `len(buf) - fade` long and its last sample leads back into
    its first, which is what stops a looping engine note clicking once a second.
    """
    f = frames(fade)
    assert len(buf) > 2 * f, "buffer too short to crossfade"
    out = buf[: len(buf) - f]
    for i in range(f):
        t = i / f
        out[i] = out[i] * t + buf[len(buf) - f + i] * (1.0 - t)
    return out


# --------------------------------------------------------------------- write


def write_wav(path, buf):
    data = b"".join(
        struct.pack("<h", max(-32768, min(32767, int(round(v * 32767.0))))) for v in buf
    )
    header = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16)
    header += b"data" + struct.pack("<I", len(data))
    return header + data


# -------------------------------------------------------------------- sounds
#
# Each of these returns a mono buffer. They are deliberately short and dry: the
# mine's sense of space comes from the mixer's distance attenuation and panning,
# and effects with their own baked-in reverb fight that.


def laser(rng):
    body = sweep(0.20, 1750, 240, shape="saw")
    top = sweep(0.20, 3300, 520, shape="sin")
    fizz = highpass(noise(0.20, rng), 2000)
    out = mix(gain(body, 0.55), gain(top, 0.35), gain(fizz, 0.20))
    return normalise(decay_env(out, 0.045))


def vulcan(rng):
    crack = decay_env(highpass(noise(0.10, rng), 1200), 0.010)
    body = decay_env(sweep(0.10, 420, 130, shape="square"), 0.022)
    thump = decay_env(tone(0.10, 85), 0.030)
    return normalise(mix(gain(crack, 0.75), gain(body, 0.40), gain(thump, 0.45)), 0.85)


def concussion(rng):
    launch = decay_env(lowpass(noise(0.45, rng), 1400), 0.13)
    whoosh = sweep(0.45, 160, 900, shape="tri")
    thud = decay_env(sweep(0.45, 150, 44), 0.07)
    out = mix(gain(launch, 0.55), gain(env(whoosh, attack=0.01, release=0.25), 0.28),
              gain(thud, 0.75))
    return normalise(out)


def homing(rng):
    base = concussion(rng)
    # The difference between the two missiles is the seeker: a thin tone that
    # rises after the launch, so you can hear which one you fired.
    whine = env(sweep(0.55, 900, 2400, shape="tri"), attack=0.12, release=0.18)
    return normalise(mix(base, gain(whine, 0.22)))


def railgun(rng):
    # A capacitor bank letting go: a hard transient, a metallic ring sliding
    # down behind it, and a low thump underneath so it lands heavier than the
    # laser it sits beside on the weapon wheel.
    crack = decay_env(highpass(noise(0.40, rng), 2600), 0.006)
    ring = decay_env(sweep(0.40, 2800, 320, shape="tri"), 0.11)
    coil = decay_env(sweep(0.40, 900, 120, shape="square"), 0.05)
    thump = decay_env(tone(0.40, 58), 0.09)
    out = mix(gain(crack, 0.85), gain(ring, 0.38), gain(coil, 0.22), gain(thump, 0.55))
    return normalise(out)


def railgun_charge(rng):
    # One pulse of a shoulder railgun building up. The robot re-emits this
    # faster and louder as the charge fills - from about twice a second to
    # eight times - so it is short, rises within itself, and has a soft tail
    # that stacks into a climbing whine instead of a stutter.
    whine = sweep(0.22, 520, 1250, shape="tri")
    for i, sample in enumerate(whine):
        whine[i] = sample * (0.7 + 0.3 * math.sin(2.0 * math.pi * 61.0 * i / RATE))
    hum = sweep(0.22, 110, 160, shape="square")
    fizz = highpass(noise(0.22, rng), 3500)
    out = mix(gain(env(whine, attack=0.03, release=0.10), 0.60),
              gain(env(lowpass(hum, 900), attack=0.02, release=0.10), 0.30),
              gain(env(fizz, attack=0.05, release=0.08), 0.10))
    return normalise(out, 0.80)


def railgun_break(rng):
    # A shoulder railgun torn off its mount: a hard snap, a bright clang with
    # no pitch the other metal sounds share, the charge bank shorting out in a
    # falling fizz, and something heavy tumbling away underneath. Long enough
    # to be heard over the fight it happens in.
    snap = decay_env(highpass(noise(0.95, rng), 2200), 0.008)
    clang = mix(
        decay_env(tone(0.95, 587, shape="tri"), 0.16),
        decay_env(tone(0.95, 1391, shape="sin"), 0.10),
        decay_env(tone(0.95, 2263, shape="sin"), 0.06),
    )
    short = highpass(noise(0.95, rng), 1200)
    for i, sample in enumerate(short):
        short[i] = sample * (0.5 + 0.5 * math.sin(2.0 * math.pi * 33.0 * i / RATE))
    zap = decay_env(sweep(0.95, 1900, 140, shape="saw"), 0.22)
    thud = decay_env(sweep(0.95, 130, 45), 0.12)
    out = mix(gain(snap, 0.80), gain(clang, 0.55), gain(decay_env(short, 0.25), 0.30),
              gain(zap, 0.28), gain(thud, 0.60))
    return normalise(out)


def lightning(rng):
    # One burst of arc. Short and soft-edged on purpose: the gun re-triggers
    # this every fireInterval for as long as the trigger is held, so anything
    # longer or harder stacks into a drone instead of reading as a crackle.
    arc = highpass(noise(0.22, rng), 1500)
    for i, sample in enumerate(arc):
        # Amplitude modulation is what makes noise crackle rather than hiss.
        arc[i] = sample * (0.55 + 0.45 * math.sin(2.0 * math.pi * 47.0 * i / RATE))
    body = decay_env(sweep(0.22, 260, 175, shape="square"), 0.06)
    out = mix(gain(env(arc, attack=0.004, release=0.07), 0.80), gain(body, 0.30))
    return normalise(out, 0.80)


def flare(rng):
    pop = decay_env(noise(0.06, rng), 0.012)
    hiss = env(lowpass(noise(0.30, rng), 3800), attack=0.01, sustain=0.5, release=0.22)
    return normalise(mix(gain(pop, 0.6), gain(hiss, 0.35)), 0.7)


def empty(rng):
    click = decay_env(highpass(noise(0.05, rng), 2600), 0.006)
    tick = decay_env(tone(0.05, 950, shape="square"), 0.008)
    return normalise(mix(gain(click, 0.7), gain(tick, 0.3)), 0.6)


def select(_rng):
    a = env(tone(0.045, 1180, shape="square"), attack=0.001, release=0.02)
    b = env(tone(0.045, 1760, shape="square"), attack=0.001, release=0.025)
    return normalise(concat(gain(a, 0.5), gain(b, 0.5)), 0.55)


def impact(rng):
    tick = decay_env(highpass(noise(0.16, rng), 3000), 0.010)
    body = decay_env(lowpass(noise(0.16, rng), 900), 0.035)
    ring = decay_env(tone(0.16, 320), 0.030)
    return normalise(mix(gain(tick, 0.55), gain(body, 0.5), gain(ring, 0.25)), 0.8)


def explosion(rng, seconds=0.9, low=52):
    crack = decay_env(noise(seconds, rng), 0.05)
    body = decay_env(lowpass(noise(seconds, rng), 700), 0.22)
    boom = decay_env(sweep(seconds, low * 2.6, low), 0.26)
    rumble = decay_env(lowpass(noise(seconds, rng), 160), 0.40)
    out = mix(gain(crack, 0.35), gain(body, 0.6), gain(boom, 0.8), gain(rumble, 0.5))
    return normalise(out)


def robot_hit(rng):
    clang = mix(
        decay_env(tone(0.28, 740, shape="tri"), 0.055),
        decay_env(tone(0.28, 1113, shape="tri"), 0.040),
        decay_env(tone(0.28, 1620, shape="sin"), 0.028),
    )
    scrape = decay_env(highpass(noise(0.28, rng), 1800), 0.020)
    return normalise(mix(gain(clang, 0.55), gain(scrape, 0.45)), 0.8)


def robot_fire(rng):
    body = sweep(0.24, 900, 160, shape="square")
    grit = decay_env(lowpass(noise(0.24, rng), 2200), 0.05)
    return normalise(mix(gain(decay_env(body, 0.06), 0.55), gain(grit, 0.4)), 0.8)


def robot_alert(_rng):
    a = env(tone(0.10, 520, shape="square"), attack=0.004, release=0.04)
    b = env(tone(0.13, 780, shape="square"), attack=0.004, release=0.06)
    return normalise(concat(gain(a, 0.5), gain(b, 0.5)), 0.7)


def player_hurt(rng):
    thud = decay_env(sweep(0.35, 220, 60), 0.07)
    shield = decay_env(highpass(noise(0.35, rng), 1500), 0.09)
    buzz = decay_env(tone(0.35, 140, shape="saw"), 0.06)
    return normalise(mix(gain(thud, 0.7), gain(shield, 0.35), gain(buzz, 0.3)))


def player_death(rng):
    blast = explosion(rng, seconds=1.4, low=40)
    fall = env(sweep(1.4, 620, 70, shape="tri"), attack=0.01, release=0.6)
    return normalise(mix(gain(blast, 0.85), gain(fall, 0.3)))


def door_open(rng):
    servo = env(sweep(0.75, 90, 190, shape="saw"), attack=0.06, sustain=0.9, release=0.20)
    grind = env(lowpass(noise(0.75, rng), 600), attack=0.06, sustain=0.8, release=0.22)
    clunk = concat(silence(0.68), decay_env(lowpass(noise(0.12, rng), 320), 0.035))
    return normalise(mix(gain(servo, 0.35), gain(grind, 0.5), gain(clunk, 0.7)), 0.8)


def door_close(rng):
    servo = env(sweep(0.70, 190, 85, shape="saw"), attack=0.05, sustain=0.9, release=0.10)
    grind = env(lowpass(noise(0.70, rng), 560), attack=0.05, sustain=0.8, release=0.12)
    slam = concat(silence(0.62), decay_env(lowpass(noise(0.20, rng), 260), 0.055))
    return normalise(mix(gain(servo, 0.32), gain(grind, 0.45), gain(slam, 0.9)), 0.85)


def locked(_rng):
    buzz = env(tone(0.30, 165, shape="square"), attack=0.003, release=0.05)
    # Gated into two short blats, which reads as "no" far better than one tone.
    n = len(buzz)
    for i in range(n):
        if frames(0.12) <= i < frames(0.17):
            buzz[i] = 0.0
    return normalise(buzz, 0.65)


def pickup(_rng):
    a = env(tone(0.07, 880, shape="tri"), attack=0.002, release=0.03)
    b = env(tone(0.09, 1320, shape="tri"), attack=0.002, release=0.05)
    return normalise(concat(gain(a, 0.5), gain(b, 0.5)), 0.7)


def key_pickup(_rng):
    notes = [659.25, 830.61, 987.77, 1318.51]   # an E major arpeggio
    out = []
    for i, f in enumerate(notes):
        seg = env(tone(0.11, f, shape="tri"), attack=0.003, release=0.06)
        out = concat(out, gain(seg, 0.45 + 0.05 * i))
    return normalise(out, 0.8)


def reactor(rng):
    blast = explosion(rng, seconds=2.2, low=34)
    # A sub-bass drop under the blast: the mine's power going out.
    drop = env(sweep(2.2, 180, 28), attack=0.02, release=1.1)
    tear = decay_env(lowpass(noise(2.2, rng), 260), 0.85)
    return normalise(mix(gain(blast, 0.8), gain(drop, 0.55), gain(tear, 0.4)))


def countdown(_rng):
    beep = env(tone(0.16, 1046.5, shape="square"), attack=0.004, release=0.07)
    return normalise(gain(beep, 0.6), 0.7)


def exit_sound(_rng):
    notes = [523.25, 659.25, 783.99, 1046.5]
    out = silence(0.0)
    for i, f in enumerate(notes):
        seg = env(tone(1.2 - 0.18 * i, f, shape="tri"), attack=0.01, release=0.5)
        out = mix(out, concat(silence(0.18 * i), gain(seg, 0.30)))
    return normalise(out, 0.85)


def engine(rng):
    """A seamless 1.0 s loop. Every partial is an integer number of Hz, so each
    completes a whole number of cycles in one second and meets its own start."""
    extra = 0.06
    length = 1.0 + extra
    layers = [
        gain(tone(length, 48, shape="saw"), 0.35),
        gain(tone(length, 72, shape="saw"), 0.22),
        gain(tone(length, 96, shape="sin"), 0.18),
        gain(tone(length, 143, shape="sin"), 0.10),
        gain(lowpass(noise(length, rng), 420), 0.30),
    ]
    return normalise(seamless(mix(*layers), extra), 0.55)


def alarm(rng):
    """A seamless 1.2 s two-tone klaxon."""
    extra = 0.06
    piece_a = env(tone(0.42, 440, shape="square"), attack=0.02, release=0.12)
    piece_b = env(tone(0.42, 330, shape="square"), attack=0.02, release=0.12)
    body = concat(
        gain(piece_a, 0.45), silence(0.18), gain(piece_b, 0.45), silence(0.18)
    )
    body = concat(body, body[: frames(extra)])
    rumble = gain(lowpass(noise(len(body) / RATE, rng), 190), 0.12)
    return normalise(seamless(mix(body, rumble), extra), 0.62)


SOUNDS = {
    "laser.wav": laser,
    "vulcan.wav": vulcan,
    "concussion.wav": concussion,
    "homing.wav": homing,
    "flare.wav": flare,
    "empty.wav": empty,
    "select.wav": select,
    "impact.wav": impact,
    "explosion.wav": explosion,
    "robot_hit.wav": robot_hit,
    "robot_fire.wav": robot_fire,
    "robot_alert.wav": robot_alert,
    "player_hurt.wav": player_hurt,
    "player_death.wav": player_death,
    "door_open.wav": door_open,
    "door_close.wav": door_close,
    "locked.wav": locked,
    "pickup.wav": pickup,
    "key_pickup.wav": key_pickup,
    "reactor.wav": reactor,
    "countdown.wav": countdown,
    "railgun.wav": railgun,
    "railgun_charge.wav": railgun_charge,
    "railgun_break.wav": railgun_break,
    "lightning.wav": lightning,
    "exit.wav": exit_sound,
    "engine.wav": engine,
    "alarm.wav": alarm,
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--out",
        default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "sounds"),
    )
    parser.add_argument("--check", action="store_true",
                        help="do not write; exit non-zero if any file differs")
    args = parser.parse_args()
    out = os.path.abspath(args.out)
    if not args.check:
        os.makedirs(out, exist_ok=True)

    differences = 0
    for name, make in sorted(SOUNDS.items()):
        # Seeded per sound, per name: adding a sound must not change the noise
        # in every sound written after it, or every regeneration would rewrite
        # the whole directory.
        rng = random.Random(name)
        blob = write_wav(name, make(rng))
        path = os.path.join(out, name)
        if args.check:
            existing = open(path, "rb").read() if os.path.exists(path) else None
            if existing != blob:
                print("differs: {}".format(name))
                differences += 1
            continue
        with open(path, "wb") as f:
            f.write(blob)
        print("{:>18}  {:>7} bytes  {:.2f}s".format(name, len(blob), (len(blob) - 44) / 2 / RATE))

    if args.check:
        print("{} file(s) differ".format(differences))
        return 1 if differences else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
