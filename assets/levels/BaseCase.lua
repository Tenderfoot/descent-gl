-- Prologue: the player comes to disabled in the reactor room. Three
-- transmissions come in back to back, and the last one hands the ship back.

loadout {
  shields = 10,     -- under the 10% threshold: disabled, drifting, tumbling
  energy = 0,
  vulcan = 0,
  concussion = 1,   -- the one missile for the reactor
  homing = 0,
  railgun = 0, 
  lightning = 0, 
  flares = 0,
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
  id = "help",
  clip = "help_each_other",
  speaker = "???",
  text = "I'm going to restore control to your ship. My readings indicate you are energy depleted, but have one missile on deck.\n\nMake it count...\n\nAnd maybe we can help each other",
  seconds = 10,
}

tutorial {
  id = "shipcontroltutorial",
  clip = "shipcontroltutorial",
  title = "Flight Controls",
  text = "W/S - Forward/Back\nA/D - Strafe left, Strafe Right\nQ/E - Roll\nLeft Click - Primary Fire\nRight Click - Secondary Fire (Missiles)\nG - Toggle Missile Type",
}

-- The whole opening hangs off one timer. after() is counted on the level
-- clock, so the sequence does not run on while the game is paused or while a
-- tutorial pane holds the simulation, and a restart drops the pending beats
-- and replays it from the top.
--
--   2.0 .. 9.0   hear_your_sos
--  14.0 .. 21.0  canthitthisfrequency   (5s of silence between)
--  26.0 .. 36.0  help_each_other        (5s of silence between)
--  36.0          ship back, then the controls pane
timer {
  at = 2.0,
  on_fire = function()
    comm("sos")
    after(12.0, function()          -- 7s clip + 5s buffer
      comm("frequency")
      after(12.0, function()        -- 7s clip + 5s buffer
        comm("help")
        after(10.0, function()      -- lands as the last line ends
          set_shields(22)           -- back over the 10% cutoff: control returns
          show_tutorial("shipcontroltutorial")
        end)
      end)
    end)
  end,
}