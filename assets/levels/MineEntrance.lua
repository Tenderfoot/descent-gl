loadout {
  shields = 100,
  energy = 100,
  vulcan = 0,
  concussion = 0,   
  homing = 0,
  flares = 20,
  railgun = 5, 
  lightning = 10, 
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

message {
  id = "firstbluekeypickup",
  clip = "firstbluekeypickup",
  speaker = "???",
  text = "Nice! You found a blue security key.\n But this is a dead end.\nYou'll need to use your automap to find the matching door.",
  seconds = 10,
}

message {
  id = "last_key_mine_1",
  clip = "last_key_mine_1",
  speaker = "???",
  text = "That should be the last key for this mine. Now you just need to find and destroy the reactor.",
  seconds = 7,
}

message {
  id = "reactorsblown",
  clip = "reactorsblown",
  speaker = "???",
  text = "That's the reactor!\nThe whole place is coming down.\nGet to the exit - move!",
  seconds = 5.0,
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
    comm("firstbluekeypickup")
    after(10, function() show_tutorial("automapinfo") end)
  end,
}

pickup {
  kind = "key_purple",
  on_pickup = function()
    comm("last_key_mine_1")
  end,
}

event {
  kind = "reactor_destroyed",
  on_event = function()
    comm("reactorsblown")
  end,
}