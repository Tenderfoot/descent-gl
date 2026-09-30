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
  id = "undefinedbehaviour_start",
  clip = "undefinedbehaviour_start",
  speaker = "???",
  text = "I've registered you in our system. Your designation is Agrajag. It's from a book I read as a girl.",
  seconds = 8,
}

message {
  id = "undefinedbehaviour_key_purple",
  clip = "undefinedbehaviour_key_purple",
  speaker = "???",
  text = "It's crazy to think these tunnels exist because they used to extract metals and minerals.",
  seconds = 8.5,
}

message {
  id = "undefinedbehaviour_key_blue",
  clip = "undefinedbehaviour_key_blue",
  speaker = "???",
  text = "I tried to pin down exactly where you came from. I couldn't find anything. You didn't want to give me a clue, did you?",
  seconds = 10.5,
}

message {
  id = "undefinedbehaviour_segment68",
  clip = "undefinedbehaviour_segment68",
  speaker = "???",
  text = "The emergency hatch is here.",
  seconds = 2.5,
}

message {
  id = "undefinedbehaviour_key_yellow",
  clip = "undefinedbehaviour_key_yellow",
  speaker = "???",
  text = "You know, most of us know people in those mines. That's why we're out here.",
  seconds = 6.5,
}

message {
  id = "undefinedbehaviour_reactor_destroyed",
  clip = "undefinedbehaviour_reactor_destroyed",
  speaker = "???",
  text = "I'm reading a huge data spike centered on your ship. What are you downloading? And how can you process that much data?\nYou're just full of secrets.",
  seconds = 8.5,
}

timer {
  at = 2.0,
  on_fire = function()
    comm("undefinedbehaviour_start")
  end,
}

pickup {
  kind = "key_purple",
  on_pickup = function()
    comm("undefinedbehaviour_key_purple")
  end,
}

pickup {
  kind = "key_blue",
  on_pickup = function()
    comm("undefinedbehaviour_key_blue")
  end,
}

trigger {
  segment = 68,
  once = true,
  on_enter = function()
    comm("undefinedbehaviour_segment68")
  end,
}

pickup {
  kind = "key_yellow",
  on_pickup = function()
    comm("undefinedbehaviour_key_yellow")
  end,
}

event {
  kind = "reactor_destroyed",
  on_event = function()
    comm("undefinedbehaviour_reactor_destroyed")
  end,
}
