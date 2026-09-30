-- Shields and energy are left out, so they start at the ship's defaults.
loadout {
  vulcan = 0,
  concussion = 0,
  homing = 0,
  railgun = 0,
  lightning = 0,
  flares = 0,
}

message {
  id = "bufferoverflow_start",
  clip = "bufferoverflow_start",
  speaker = "???",
  text = "You don't take breaks? Or need to sleep? I could give you coordinates to get a new paint job. Not interested?",
  seconds = 8,
}

message {
  id = "bufferoverflow_key_blue",
  clip = "bufferoverflow_key_blue",
  speaker = "???",
  text = "We haven't been able to take out a mine in months. Are you about to do four in a day?",
  seconds = 8,
}

message {
  id = "bufferoverflow_key_yellow",
  clip = "bufferoverflow_key_yellow",
  speaker = "???",
  text = "There are people who think what we're doing is cruel. They compare it to capital punishment.\n\nWe all know it's mercy.",
  seconds = 11.5,
}

message {
  id = "bufferoverflow_key_purple",
  clip = "bufferoverflow_key_purple",
  speaker = "???",
  text = "The reactor is next. Do you remember where the hatch was?",
  seconds = 8,
}

message {
  id = "bufferoverflow_reactor_destroyed",
  clip = "bufferoverflow_reactor_destroyed",
  speaker = "???",
  text = "And there's that data transfer again. Are you a corpo agent? An alien? An AI?\nShould I even be helping you?",
  seconds = 11,
}

timer {
  at = 2.0,
  on_fire = function()
    comm("bufferoverflow_start")
  end,
}

pickup {
  kind = "key_blue",
  on_pickup = function()
    comm("bufferoverflow_key_blue")
  end,
}

pickup {
  kind = "key_yellow",
  on_pickup = function()
    comm("bufferoverflow_key_yellow")
  end,
}

pickup {
  kind = "key_purple",
  on_pickup = function()
    comm("bufferoverflow_key_purple")
  end,
}

event {
  kind = "reactor_destroyed",
  on_event = function()
    comm("bufferoverflow_reactor_destroyed")
  end,
}
