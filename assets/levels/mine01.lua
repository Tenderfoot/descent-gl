message {
  id = "lower_mine",
  clip = "intro",
  speaker = "???",
  text = "...Shit!",
  seconds = 5.0,
}

message {
  id = "reactor_approach",
  clip = "catangry",
  speaker = "???",
  text = "Get off this frequency! I don't know how you got these hertz, but jump off!",
  seconds = 5.0,
}

trigger {
  segment = 8,
  once = true,
  on_enter = function()
    comm("lower_mine")
  end,
}

trigger {
  segment = 12,
  once = true,
  on_enter = function()
    comm("reactor_approach")
  end,
}
