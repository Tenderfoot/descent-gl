#include "game/level_switch_test.h"

#include "core/log.h"

#include <glm/vec2.hpp>

namespace dvk {

MenuInput LevelSwitchTest::menuInput(int frame) {
    MenuInput input;
    input.pointer = glm::vec2(-1.0f);
    input.next = frame == kPickFrame;
    input.confirm = frame == kConfirmFrame;
    return input;
}

int LevelSwitchTest::report(const std::string& toLevel, int toSegments, bool playing,
                            int shipSegment, long long steps) const {
    const bool changed = toLevel != fromLevel;
    const bool inside = shipSegment >= 0 && shipSegment < toSegments;
    const bool stepped = steps > 0;
    log::info(
        "level-switch-test: started on '{}' ({} segments), ended on '{}' ({} segments); "
        "playing {}, ship in segment {} of {}, {} sim steps in the new mine",
        fromLevel, fromSegments, toLevel, toSegments, playing ? "yes" : "no", shipSegment,
        toSegments, steps);
    if (!changed) log::error("level-switch-test: the level list started no new level");
    if (!playing) log::error("level-switch-test: the new level is not being played");
    if (!inside) log::error("level-switch-test: the ship is not inside the new level");
    if (!stepped) log::error("level-switch-test: the new level ran no simulation steps");
    const bool ok = changed && playing && inside && stepped;
    log::info("level switch: {}", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace dvk
