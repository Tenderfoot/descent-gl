message {
  id = "anothermine",
  clip = "anothermine",
  speaker = "Mysterious Cat",
  text = "You've made your way to another mine. Good. I'll help you through this one.",
  seconds = 5,
}

trigger {
  segment = 1,
  once = true,
  on_enter = function()
    comm("anothermine")
  end,
}