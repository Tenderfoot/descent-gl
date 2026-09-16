message {
  id = "lower_mine",
  clip = "intro",
  speaker = "MINE CONTROL",
  text = "The lower mine is unstable. Watch the keyed bulkheads and keep an exit route open.",
  seconds = 5.5,
}

message {
  id = "reactor_approach",
  clip = "intro",
  speaker = "MINE CONTROL",
  text = "Reactor signatures are ahead. Once it goes, the whole mine goes with it.",
  seconds = 5.0,
}

trigger {
  segment = 12,
  once = true,
  on_enter = function()
    comm("lower_mine")
  end,
}

trigger {
  segment = 30,
  once = true,
  on_enter = function()
    comm("reactor_approach")
  end,
}
