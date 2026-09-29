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
- **What a kill drops depends on the weapon that landed the last hit.** Every
  type is `dropNothing: 1.0`, so a kill by anything else drops nothing. The
  reactor is exempt from both drops below. Dropped pickups are the level's own
  pickups and give the same amounts.
  - **Static Charge finish:** an energy pickup where the robot died, plus one
    pickup of each kind in the type's `chargeDrops`, scattered 2 units around
    it.
  - **Player missile kill** (concussion or homing, by impact or blast): a
    shield pickup (+25) where the robot died. The robot does not have to be
    disabled first, so a drone one-shot by a missile drops one too. A robot
    missile's blast does not count, even one the player shot down.

  | Robot | Static Charge finish drops |
  |---|---|
  | drone | energy |
  | gunner | energy, vulcan |
  | gunner_missile | energy, homing, vulcan |
  | brute | energy, concussion |
  | brute_railgun | energy, concussion, railgun |
- **There is no energy or shield regeneration.** `energyRecharge` is 0;
  energy comes from pickups and Static Charge finishes, shields from pickups
  and missile kills. The headlight drains 0.2/s.

## The player

| | Value |
|---|---|
| Shields / energy | 100 / 100 (disabled at 10 shields) |
| Shield pickup / energy pickup | +25 / +30 |
| Default ammo | vulcan 2000, concussion 8, homing 6, railgun 20, Static Charge 10, flares 12 |
| Ammo pickups | concussion +3, homing +3, railgun +3, vulcan +200, flares +4; Static Charge fills to 10 |

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

A special attack starts whenever the robot is on to the player and can see
them, at any distance and in any mode. Attack range governs only the ordinary
bursts; the special has no range test beyond its own shot's reach (the
railgun's 400) - line of sight and awareness are the only gates, the same ones
that decide whether a robot notices the player at all. A robot that has never
spotted the player in the dark still cannot start one.

Once its wind-up starts it always fires - out of Attack, or out of sight (at
the last place it saw the player) - unless the attachment is knocked off or
the robot is disabled or killed. While it winds up outside Attack, the robot
turns to aim at the player without changing course. Its ordinary bursts carry
on throughout, within their own range.

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
- **Long sightlines are now special-attack range.** With no distance gate, a
  brute_railgun that has noticed the player fires every 3.75 s from anywhere
  it can see them, and a gunner_missile launches a homing missile every 8 s -
  from well beyond where the player's lasers and missiles are practical.
  The answers are breaking line of sight and the player's railgun (range 300,
  and the only weapon that knocks the attachment off). Watch long, lit
  galleries in the later levels, and whether the railgun becomes mandatory
  rather than rewarding.
- **Laser levels 3 and 4 do nothing against drones.** From level 2 up a drone
  always takes three shots; at level 4 the second would kill it, so the laser
  rule adds the third. Drone health of about 30-35 would let the top levels
  show against the most common enemy.
- **Level 1 laser is a long fight against anything big.** 16-23 consecutive
  hits on a strafing gunner or brute, with no energy regeneration. How much
  this matters depends on how early levels hand out laser upgrades.
- **Rail ammo in MineEntrance.** It starts with 5 rounds - under two brutes'
  worth even with knock-offs.
- **The Static Charge finish is the resupply loop, by design.** Bring a robot
  low, switch, and finish it with the Static Charge to get its energy and
  ammunition. From 1 health (after the laser rule) the finish costs a
  fraction of a round; from a brute's 19 after three rails it costs about
  2.6 rounds. So how a robot is brought down decides how many finishes a
  10-round charge buys, and the laser's overshoot gates the cheap version.
  Watch whether a player who has learned it stops being short of anything:
  a brute_railgun finish returns +30 energy, +3 concussion and +3 rail -
  about as much rail as the kill cost.
- **Missile kills are a shield source, capped only by missile supply.**
  Drones die to one missile, so every drone is a missile for +25 shields, and
  brutes drop concussions on a Static Charge finish, so the two loops can feed
  each other: finish a brute with the charge for three concussions, spend them
  on drones for 75 shields. Shields recovered mid-fight also blunt incoming
  pressure, the brute_railgun above most of all. Watch whether shields stop
  being the resource that ends a run; requiring the robot to be disabled
  first, or not counting drones, are the levers if so.
- **The Static Charge is a finisher, not a killer.** A full charge is 72
  damage: a drone, not a gunner.
- **Stale fallback defaults in code.** See the top of this file.
