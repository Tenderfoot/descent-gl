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
  id = "segmentationfault_start",
  clip = "segmentationfault_start",
  speaker = "???",
  text = "Here we go again. There's a crowd around on my side.\nEveryone here is curious what you're going to do next.",
  seconds = 7,
}

message {
  id = "segmentationfault_key_purple",
  clip = "segmentationfault_key_purple",
  speaker = "???",
  text = "This mine is a torus shape. I guess... like a doughnut. Do you know doughnuts? If I ever get to go to Earth, first thing I'm doin' is trying a doughnut.",
  seconds = 16,
}

message {
  id = "segmentationfault_key_yellow",
  clip = "segmentationfault_key_yellow",
  speaker = "???",
  text = "I know it's probably not easy. You can't feel bad about what you're doing. It's the right thing, grim as it is.",
  seconds = 16,
}

message {
  id = "segmentationfault_key_blue",
  clip = "segmentationfault_key_blue",
  speaker = "???",
  text = "The reactor is the last step. Bringing down this reactor will have real impact on Earth.\nAvailable processing power will hit critical levels. There will be riots.",
  seconds = 16,
}

message {
  id = "segmentationfault_reactor_destroyed",
  clip = "segmentationfault_reactor_destroyed",
  speaker = "???",
  text = "It's on the news. They know about you. I'll be honest, opinions are mixed. I did see some positive graffiti.",
  seconds = 12,
}

timer {
  at = 2.0,
  on_fire = function()
    comm("segmentationfault_start")
  end,
}

pickup {
  kind = "key_purple",
  on_pickup = function()
    comm("segmentationfault_key_purple")
  end,
}

pickup {
  kind = "key_yellow",
  on_pickup = function()
    comm("segmentationfault_key_yellow")
  end,
}

pickup {
  kind = "key_blue",
  on_pickup = function()
    comm("segmentationfault_key_blue")
  end,
}

event {
  kind = "reactor_destroyed",
  on_event = function()
    comm("segmentationfault_reactor_destroyed")
  end,
}
