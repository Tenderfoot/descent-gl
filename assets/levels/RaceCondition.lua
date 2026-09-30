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
  id = "racecondition_start",
  clip = "racecondition_start",
  speaker = "???",
  text = "They've already retaken Paris. There are two penal mines left. Two more targets. You're going to put me out of a job.",
  seconds = 13,
}

message {
  id = "racecondition_key_purple",
  clip = "racecondition_key_purple",
  speaker = "???",
  text = "We know you aren't human. It's physiologically impossible for you to have been piloting this long.",
  seconds = 8,
}

message {
  id = "racecondition_key_yellow",
  clip = "racecondition_key_yellow",
  speaker = "???",
  text = "They have a theory on Earth. They say you're being run by the same net you're destroying.\nBut your efficiency hasn't gone down. In fact, it's been the opposite.",
  seconds = 16,
}

message {
  id = "racecondition_key_blue",
  clip = "racecondition_key_blue",
  speaker = "???",
  text = "The neural nets in these mines were providing the vast majority of processing power for Earth.\nPeople will need to learn to live without it. It will be hard, at first, but it's necessary.",
  seconds = 12,
}

message {
  id = "racecondition_reactor_destroyed",
  clip = "racecondition_reactor_destroyed",
  speaker = "???",
  text = "There's one mine left after this. I suppose you're headed there next? I'll get someone to put on some coffee.",
  seconds = 10.5,
}

timer {
  at = 2.0,
  on_fire = function()
    comm("racecondition_start")
  end,
}

pickup {
  kind = "key_purple",
  on_pickup = function()
    comm("racecondition_key_purple")
  end,
}

pickup {
  kind = "key_yellow",
  on_pickup = function()
    comm("racecondition_key_yellow")
  end,
}

pickup {
  kind = "key_blue",
  on_pickup = function()
    comm("racecondition_key_blue")
  end,
}

event {
  kind = "reactor_destroyed",
  on_event = function()
    comm("racecondition_reactor_destroyed")
  end,
}
