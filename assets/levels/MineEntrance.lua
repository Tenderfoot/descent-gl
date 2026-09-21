loadout {
  shields = 100,
  energy = 100,
  vulcan = 0,
  concussion = 0,   
  homing = 0,
  flares = 20,
}

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

tutorial {
  id = "projvelinherit",
  clip = "projvelinherit",
  title = "Velocity Inheritance",
  text = "When firing your primary weapon projectiles, they'll inherit your velocity. You'll need to compensate with your crosshair.\n\nWhen using secondary fire missiles, they fire straight toward their trajectory.",
}

tutorial {
  id = "automapinfo",
  clip = "automapinfo",
  title = "Automap Navigation",
  text = "Press TAB to access your automap.\n\nYour mousewheel will zoom you in and out of your ships position.\n\nIt Can help you find areas you haven't yet accessed.",
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

pickup {
  kind = "concussion",
  once = true,              
  on_pickup = function(kind)
    show_tutorial("projvelinherit")
  end,
}

pickup {
  kind = "key_blue",
  on_pickup = function()
    after(5, function() show_tutorial("automapinfo") end)
  end,
}