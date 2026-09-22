-- Prologue: the player comes to disabled in the reactor room. Three
-- transmissions come in back to back, and the last one hands the ship back.

loadout {
  shields = 5,      -- under the 10% threshold: disabled, drifting, tumbling
  energy = 0,
  vulcan = 0,
  concussion = 1,   -- the one missile for the reactor
  homing = 0,
  flares = 0,
}

message {
  id = "sos",
  clip = "hear_your_sos",
  speaker = "???",
  text = "We hear your SOS.\nStay with me - you're not alone out there.",
  seconds = 7,
}

message {
  id = "frequency",
  clip = "canthitthisfrequency",
  speaker = "???",
  text = "They can't hit this frequency.\nWhatever they told you, nobody is coming for you.",
  seconds = 7,
}

message {
  id = "help",
  clip = "help_each_other",
  speaker = "???",
  text = "Out here we help each other.\nI'm bringing your systems back up.\nPut that missile into the reactor and run for the hatch.",
  seconds = 10,
}

tutorial {
  id = "shipcontroltutorial",
  clip = "shipcontroltutorial",
  title = "Flight Controls",
  text = "Your ship answers again.\n\nThrust with W and S, slide with A and D, and roll with Q and E.\n\nThe mouse pitches and yaws. Six degrees of freedom - there is no up in here.",
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
          set_shields(11)           -- back over the 10% cutoff: control returns
          show_tutorial("shipcontroltutorial")
        end)
      end)
    end)
  end,
}