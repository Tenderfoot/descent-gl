-- test01 is the hand-editable grid mine, and it is where the tutorial panes
-- are exercised. Nothing here is level content for its own sake: the triggers
-- sit on the first segment the player reaches and on one of the three the
-- level deliberately leaves unlit, so both panes are a few seconds of flying
-- apart.
--
-- Kept out of mine01 on purpose. A tutorial stops the simulation until someone
-- clicks the button, and mine01 is the level the frame-limited runs fly.

tutorial {
  id = "flight",
  clip = "testcard",
  title = "six degrees of freedom",
  text = "You are not on rails. The ship slides as well as turns: strafe left and right, up and down, and roll to put a corridor where you want it rather than flying it sideways.\n\nThe mouse aims. Everything you fly with is on the controls screen, and every one of them can be rebound.",
}

tutorial {
  id = "darkness",
  clip = "testcard",
  title = "the dark is cover",
  text = "Robots have to see you to shoot at you, and most of this mine is unlit.\n\nYour headlight costs energy and gives your position away to everything with a line of sight. Flares are the other answer: thrown light that is somewhere you are not.",
}

trigger {
  segment = 1,
  once = true,
  on_enter = function()
    show_tutorial("flight")
  end,
}

trigger {
  segment = 4,
  once = true,
  on_enter = function()
    show_tutorial("darkness")
  end,
}
