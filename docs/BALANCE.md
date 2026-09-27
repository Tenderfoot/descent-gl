# Game balance

Where the numbers stand, what they are meant to achieve, and what to watch.
Every figure here is worked out from the config and the damage code, not
measured in play; treat the "watch" items as the first things to playtest.

The live numbers are in `assets/config/robots.json` and
`assets/config/weapons.json`. The code carries fallback defaults
(`RobotConfig::defaults()` in `src/sim/robots.cpp`, `WeaponConfig::defaults()`
in `src/sim/weapons.cpp`) that only apply when a file or a field is missing.
Some of those defaults are stale: robot health, the brute railgun's timings and
the missile damages there are the values from before this pass. The railgun and
the Static Charge have no entry in `weapons.json`, so their code defaults *are*
the live values.

## Rules that shape every number

- **Disabled at 10%.** A robot at or below 10% of its full health, or the
  player at or below 10 shields, is disabled: no steering, no firing, drifting
  and tumbling. It can still be finished off. (`kDisabledHealthFraction`,
  `src/sim/body.h`.)
- **The laser never kills outright.** A player laser bolt, at any level, that
  would take a robot that is not yet disabled to zero leaves it on 1 health
  instead; the next hit of anything kills it. "Not yet disabled" is judged by
  the robot's mode, so it has to have spent a step disabled first. In practice
  a player spraying bolts will often overshoot - two or three bolts are in the
  air when the disabling one lands - and that is accepted: stopping at the
  right moment is a skill, not a guarantee. Includes the reactor.
- **Knocking a weapon off costs the robot half its full health.** A direct
  railgun hit on a mounted attachment knocks it loose, ends its special
  attack, and deals `breakDamage` (a fraction of full health, per type, 0.5 on
  both current attachments). It can kill a robot that was already low. Any
  other weapon hitting the attachment counts as a hit on the robot. The
  attachment hitboxes are drawn in game (settings > game > weapon hitboxes, on
  by default) so the shot can be learned.
- **Direct missile hits also take their own blast.** A missile does its impact
  damage plus splash, measured from where it struck to the robot's centre, so
  a big robot takes less of the blast than a small one.
- **Splash falls off linearly** to zero at the blast radius, does not pass
  through rock, and hurts the player and other robots too. The blast is shown
  in game: the explosion light reaches exactly the blast radius, and a
  shockwave ring grows out to it.
- **Robots drop nothing on their own.** Every type is `dropNothing: 1.0`. A
  robot finished by the Static Charge always drops energy (+30); the reactor
  is exempt.
- **There is no energy regeneration.** `energyRecharge` is 0; energy comes
  from pickups and Static Charge finishes. The headlight drains 0.2/s.

## The player

| | Value |
|---|---|
| Shields / energy | 100 / 100 (disabled at 10 shields) |
| Shield pickup / energy pickup | +25 / +30 |
| Default ammo | vulcan 2000, concussion 8, homing 6, railgun 20, Static Charge 10, flares 12 |
| Ammo pickups | concussion +3, homing +3, railgun +10, vulcan +500, flares +4; Static Charge fills to 10 |

Levels override the defaults with a `loadout` block. MineEntrance starts with
railgun 5 and Static Charge 10; TheBeginning starts disabled with one
concussion missile.

## Player weapons

| Weapon | Damage | Rate | Cost | Notes |
|---|---|---|---|---|
| Laser | 8 / 12 / 17 / 23 by level | every 0.18 s | 0.7 energy a shot | never kills outright (see above) |
| Vulcan | 3 | every 0.05 s (60/s) | 1 round | spread 0.022 rad |
| Railgun | 55, hitscan, range 300 | every 1.15 s | 1 round | only weapon that knocks attachments off |
| Static Charge | 36 per second, hitscan, range 70 | continuous | 5 rounds per second | holds 10 (2 s of trigger, 72 damage); builds 1 round/s while flying |
| Concussion | 55 + 28 splash, radius 12 | every 0.65 s | 1 missile | speed 55 |
| Homing | 50 + 22 splash, radius 10 | every 0.8 s | 1 missile | speed 46, turns 2.6 rad/s, seeks in a 0.9 rad cone to 140 |

### Laser levels

Only damage changes with level; rate and energy cost do not, so damage per
energy scales with it too.

| Level | Damage | Step | Damage/s | drone (26) | gunner (123) | brute (184) | reactor (600) |
|---|---|---|---|---|---|---|---|
| 1 | 8 | | 44 | 4 shots | 16 (2.7 s, 11 energy) | 23 (4.0 s, 16 energy) | 75 (13.3 s, 52 energy) |
| 2 | 12 | +50% | 67 | 3 | 11 (1.8 s, 8) | 16 (2.7 s, 11) | 50 (8.8 s, 35) |
| 3 | 17 | +42% | 94 | 3 | 8 (1.3 s, 6) | 11 (1.8 s, 8) | 36 (6.3 s, 25) |
| 4 | 23 | +35% | 128 | 3 | 6 (0.9 s, 4) | 9 (1.4 s, 6) | 27 (4.7 s, 19) |

Shot counts include the laser rule, and assume every bolt hits and none
overshoots.

### Missiles against each robot

A direct hit's total, for a strike between the robot's surface and 0.6 further
out (the missile's own radius):

| Robot | Concussion per hit | To kill | Homing per hit | To kill |
|---|---|---|---|---|
| drone | 77–79 | 1 | 66–68 | 1 |
| gunner | 76.5–78 | 2 (one leaves ~46, 37%) | 66–67 | 2 (one leaves ~56, 46%) |
| brute | 74–76 | 3 (two leave ~33–35, 18%) | 64–65 | 3 (two leave ~54–56, 30%) |
| reactor | 69–70 | 9 | 59–60 | 11 |

Both missiles kill gunners in two and brutes in three with a clear margin, so
the result does not depend on exactly where the missile touches. Concussion
keeps more damage and the larger blast; homing's value is that it aims itself.

## Robots

| Type | Health | Radius | Speed | Attack range | Fire | Rough damage/s if every bolt hits |
|---|---|---|---|---|---|---|
| drone | 26 | 1.9 | 19 | 45 | 2 × 4, 0.3 s apart, 1.2 s pause | ~5 |
| gunner | 123 | 2.2 | 13 | 75 | 4 × 8, 0.22 s apart, 1.6 s pause, lead 0.75 | ~14 |
| gunner_missile | 123 | 2.2 | 13 | 75 | as gunner, plus a missile pod | ~14 + missile |
| brute | 184 | 3.1 | 8.5 | 55 | 2 × 14, 0.5 s apart, 2.0 s pause | ~11 |
| brute_railgun | 184 | 3.1 | 8.5 | 55 | as brute, plus a railgun | ~11 + ~8.5 |
| reactor | 600 | 5.5 | 0 | 130 | 2 × 16, 0.8 s apart, 1.8 s pause | ~12 |

### Tuning targets

- **Brute and brute_railgun, 184:** three railgun body shots (165) leave 19,
  10.3% - just above disabled. The fourth kills.
- **Gunner and gunner_missile, 123:** two railgun body shots (110) leave 13,
  10.6% - just above disabled. The third kills.
- **Knocking the weapon off first is the reward for accuracy.** brute_railgun:
  knock-off 92, then one rail leaves 37 (20%), a second kills - three rail
  rounds in all instead of four, and the special attack is gone after the
  first. gunner_missile: knock-off 61.5, then one rail leaves 6.5 (5.3%),
  disabled.

### Special attacks

A special attack starts only in Attack mode with the player in sight. Once its
wind-up starts it always fires - out of range, out of Attack, or out of sight
(at the last place it saw the player) - unless the attachment is knocked off
or the robot is disabled or killed. The robot does not have to face the
player. Its ordinary bursts carry on throughout.

| | brute_railgun | gunner_missile |
|---|---|---|
| Delivery | hitscan, range 400, no spread, no lead | homing projectile, speed 36, turns 1.4 rad/s, lives 7 s |
| Damage | 32, impulse 30 | 18 + 14 splash in radius 9 |
| Wind-up | 3.5 s | 3.0 s |
| Cooldown after firing | 0.25 s | 5.0 s |
| Fastest repeat | every 3.75 s | every 8 s |
| Answer | cover, or knock the railgun off | out-turn it, shoot it down (it is shootable, and its blast hurts robots near it), or knock the pod off |

## Watch list

- **brute_railgun's wind-up is nearly always on.** With a 0.25 s cooldown it
  starts winding up again almost as soon as it fires, so the orb is almost
  always present and only its size carries the warning. Three hits (96)
  disable the player - about 11 s in its sight. A cooldown of 1.5-2 s would
  give the tell back a visible gap.
- **Laser levels 3 and 4 do nothing against drones.** From level 2 up a drone
  always takes three shots; at level 4 the second would kill it, so the laser
  rule adds the third. Drone health of about 30-35 would let the top levels
  show against the most common enemy.
- **Level 1 laser is a long fight against anything big.** 16-23 consecutive
  hits on a strafing gunner or brute, with no energy regeneration. How much
  this matters depends on how early levels hand out laser upgrades.
- **Rail ammo in MineEntrance.** It starts with 5 rounds - under two brutes'
  worth even with knock-offs.
- **Laser to 1 health, then a touch of Static Charge, is an energy source.**
  A Static Charge finish always drops +30 energy and the finishing zap costs a
  fraction of a round, so a clean disable-then-zap is energy-positive
  (brute: about 16 energy of laser for 30 back). It is gated by the overshoot
  above and is accepted as a skill reward, but it is the reason energy may
  not stay scarce for a player who has learned it.
- **The Static Charge is now a finisher, not a killer.** A full charge is 72
  damage: a drone, not a gunner.
- **Stale fallback defaults in code.** See the top of this file.
