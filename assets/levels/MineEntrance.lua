tutorial {
  id = "headlightflaredemo",
  clip = "headlightflaredemo",
  title = "Headlights and Flares",
  text = "Headlights (H) can help you see in dark areas, but using them drains your energy.\n\nFlares (F) can be used, but are finite supply.",
}

tutorial {
  id = "besneaky",
  clip = "besneaky",
  title = "Be Sneaky",
  text = "If you stick to the shadows and turn of your headlight, you can get the jump on enemies.\n\nYou're safe as long as the exposure meter isn't past the line",
}

message {
  id = "anothermine",
  clip = "anothermine",
  speaker = "???",
  text = "You've made your way to another mine.\nGood.\nI'll help you through this one.",
  seconds = 5,
}

trigger {
  segment = 2,
  once = true,
  on_enter = function()
    show_tutorial("headlightflaredemo")
  end,
}

trigger {
  segment = 66,
  once = true,
  on_enter = function()
    comm("anothermine")
  end,
}

trigger {
  segment = 14,
  once = true,
  on_enter = function()
    show_tutorial("besneaky")
  end,
}