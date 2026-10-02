-- Prologue: the player comes to disabled beside a disabled brute. Three
-- transmissions come in back to back, and the last one hands the ship back.
-- From there each room teaches one thing, and the voice on the comm unlocks
-- the way on as the player learns it:
--
--   27-28  disabled brute    static charge -> its battery and a missile drop
--                            missile pickup gives the blue key
--   19-17  disabled drones   missiles -> shield drops
--                            shield pickup gives the yellow key
--   15-13  gunner hallway    velocity inheritance
--   10     the gunner        the purple key is in here
--   0      the reactor       then out through the hatch

loadout {
  shields = 10,     -- under the 10% threshold: disabled, drifting, tumbling
  energy = 0,       -- nothing for the laser: static charge is the only gun
  vulcan = 0,
  concussion = 0,   -- the missiles come from the brute
  homing = 0,
  railgun = 0,
  lightning = 3,    -- enough to finish the brute with a miss or two to spare
  flares = 0,
  self_repair = false,  -- held disabled until the script hands the ship back
}

message {
  id = "sos",
  clip = "hear_your_sos",
  speaker = "???",
  text = "Attention ship. I can hear your S.O.S. But I can't verify your identity.\n\nGive me a minute.",
  seconds = 7.5,
}

message {
  id = "frequency",
  clip = "canthitthisfrequency",
  speaker = "???",
  text = "Your ship isn't in our registry. And its coming up as a corpo cruiser?\nThis doesn't make any sense.\nYou shouldn't even be able to hit this frequency.",
  seconds = 7.5,
}

message {
  id = "restorecontrol",
  clip = "basecase_restorecontrol",
  speaker = "???",
  text = "I'm restoring control of your ship. I'm detecting a disabled brute in the room with you.\n\nYou are energy depleted - use your static charge weapon to recover the brute's battery.",
  seconds = 13.5,
}

message {
  id = "unlockthewayforward",
  clip = "basecase_unlockthewayforward",
  speaker = "???",
  text = "Excellent. I'll unlock the way forward.",
  seconds = 3.5,
}

message {
  id = "recovershields",
  clip = "basecase_recovershields",
  speaker = "???",
  text = "I'm detecting disabled drones in this room. You must have already been through here.\n\nYou can use the missiles you picked up to blow up the drones and recover some shields.",
  seconds = 14.5,
}

message {
  id = "hostilesahead",
  clip = "basecase_hostilesahead",
  speaker = "???",
  text = "I'm unlocking the way to the next room. I'm detecting hostiles, prepare for conflict.",
  seconds = 8,
}

message {
  id = "remotelyunlock",
  clip = "basecase_remotelyunlock",
  speaker = "???",
  text = "I can remotely unlock the reactor chamber from here. If you can destroy the reactor and get out the hatch, I might just have some work for you.",
  seconds = 10.5,
}

message {
  id = "escapehatch",
  clip = "basecase_escapehatchinthatroom",
  speaker = "???",
  text = "The escape hatch should be in that room!",
  seconds = 3,
}

tutorial {
  id = "shipcontroltutorial",
  clip = "shipcontroltutorial",
  title = "Flight Controls",
  text = "W/S - Forward/Back\nA/D - Strafe left, Strafe Right\nQ/E - Roll\nLeft Click - Primary Fire\nRight Click - Secondary Fire (Missiles)\nG - Toggle Missile Type",
}

tutorial {
  id = "projvelinherit",
  clip = "projvelinherit",
  title = "Velocity Inheritance",
  text = "When firing your primary weapon projectiles, they'll inherit your velocity. You'll need to compensate with your crosshair.\n\nWhen using secondary fire missiles, they fire straight toward their trajectory.",
}

-- The whole opening hangs off one timer. after() is counted on the level
-- clock, so the sequence does not run on while the game is paused or while a
-- tutorial pane holds the simulation, and a restart drops the pending beats
-- and replays it from the top.
--
--   2.0 .. 9.0   hear_your_sos
--  14.0 .. 21.0  canthitthisfrequency     (5s of silence between)
--  26.0 .. 39.5  basecase_restorecontrol  (5s of silence between)
--  39.5          ship back, then the controls pane
timer {
  at = 2.0,
  on_fire = function()
    comm("sos")
    after(12.0, function()          -- 7s clip + 5s buffer
      comm("frequency")
      after(12.0, function()        -- 7s clip + 5s buffer
        comm("restorecontrol")
        after(13.5, function()      -- lands as the last line ends
          set_shields(22)           -- back over the 10% cutoff: control returns
          set_self_repair(true)     -- from here on a disabled ship recovers itself
          show_tutorial("shipcontroltutorial")
          -- TODO ammo recovery tutorial: needs a clip and its text.
        end)
      end)
    end)
  end,
}

-- Room 1. Finishing the brute with static charge drops its battery and a
-- concussion pickup; taking the missiles is what opens the blue door.
pickup {
  kind = "concussion",
  on_pickup = function()
    comm("unlockthewayforward")
    give_key("blue")
  end,
}

-- Room 2. The drones are already disabled; a missile kill always drops shields,
-- and taking the first one opens the yellow door.
--
-- Asks has_key() rather than keeping a Lua flag: the Lua state outlives a
-- restart, and the key does not.
local function openYellow()
  if has_key("yellow") then return end
  comm("hostilesahead")
  give_key("yellow")
end

trigger {
  segment = 19,             -- first segment past the door into the drone room
  once = true,
  on_enter = function()
    comm("recovershields")
    -- TODO shields tutorial: needs a clip and its text.

    -- The fallback. A drone finished by anything but a missile drops no
    -- shields, and missiles fired at the walls are gone, so either could leave
    -- the yellow door shut for good. Once every drone is dead, open it anyway.
    local function checkDrones()
      if has_key("yellow") then return end
      if robots_alive("drone") == 0 then
        openYellow()
      else
        after(1.0, checkDrones)
      end
    end
    after(1.0, checkDrones)
  end,
}

pickup {
  kind = "shield",
  on_pickup = function()
    openYellow()
  end,
}

-- The gunner hallway, just past the yellow door.
trigger {
  segment = 15,
  once = true,
  on_enter = function()
    show_tutorial("projvelinherit")
  end,
}

-- Room 3. The purple key is in the gunner's room.
pickup {
  kind = "key_purple",
  on_pickup = function()
    comm("remotelyunlock")
  end,
}

-- On the way to the reactor, just before the door into its chamber.
trigger {
  segment = 2,
  once = true,
  on_enter = function()
    comm("escapehatch")
  end,
}
