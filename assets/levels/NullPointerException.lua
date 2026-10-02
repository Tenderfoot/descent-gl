-- The last mine. The ship comes in on a quarter of its shields and only the
-- laser, so the first half is crossed in the dark rather than fought through.
-- The invulnerability pickup restores the full ceiling, makes the ship
-- invulnerable and makes every count endless for the rest of the level.
loadout {
  max_shields = 50,
  vulcan = 0,
  concussion = 0,
  homing = 0,
  railgun = 0,
  lightning = 0,
  flares = 0,
}

-- Hooks for the level's comms. Each needs a message {} and its clip under
-- assets/comm/ before the comm() line inside it is uncommented.

timer {
  at = 2.0,
  on_fire = function()
    -- comm("nullpointerexception_start")
  end,
}

pickup {
  kind = "invulnerability",
  on_pickup = function()
    -- comm("nullpointerexception_invulnerability")
  end,
}

event {
  kind = "reactor_destroyed",
  on_event = function()
    -- comm("nullpointerexception_reactor_destroyed")
  end,
}
