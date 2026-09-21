loadout {
  shields = 5,      -- under the 10% threshold: disabled, drifting, tumbling
  energy = 40,
  vulcan = 0,
  concussion = 1,   -- the one missile for the reactor
  homing = 0,
  flares = 0,
}

timer {
  at = 12.0,
  on_fire = function() set_shields(5) end,
}