#pragma once

#include "game/menu.h"

#include <string>

namespace dvk {

// --level-switch-test. Loading a second mine touches GL resources, the session
// and every per-level cache in the frame loop, so the only honest check is to
// do it and then keep playing: move the cursor down one row, start that level,
// and fly it for long enough that the new mine is simulated and drawn rather
// than merely loaded.
//
// Needs a window, which is why it is driven from the frame loop rather than
// written as one of the headless oracles.
struct LevelSwitchTest {
    static constexpr int kPickFrame = 10;
    static constexpr int kConfirmFrame = 20;
    static constexpr int kEndFrame = 90;

    std::string fromLevel;
    int fromSegments = 0;

    // What the menu is handed instead of the player's input. The script drives
    // the cursor, so the pointer is parked off every button: under a window
    // manager the mouse sits wherever it was left, and hovering a row would
    // decide which level this test picks.
    static MenuInput menuInput(int frame);

    // Says what happened and returns the process exit code.
    int report(const std::string& toLevel, int toSegments, bool playing, int shipSegment,
               long long steps) const;
};

} // namespace dvk
