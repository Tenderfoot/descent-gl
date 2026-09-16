#include "game/comm_player.h"

#include "core/log.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace dvk {

namespace {

// Short enough that the panel does not arrive and leave as a hard rectangle,
// short enough that it is not what the player is waiting on. These belong to
// the transmission's lifetime rather than to the renderer, which now only
// draws the frame and the alpha it is handed.
constexpr float kFadeInSeconds = 0.18f;
constexpr float kFadeOutSeconds = 0.45f;

} // namespace

void CommPlayer::start(double now) {
    startSeconds_ = now;
    playing_ = true;
    held_ = false;
}

void CommPlayer::stop() {
    playing_ = false;
    held_ = false;
}

void CommPlayer::hold(double seconds) {
    heldSeconds_ = seconds;
    held_ = true;
    playing_ = true;
}

CommFrame CommPlayer::at(double now) const {
    CommFrame out;
    if (timing_.frameCount <= 0 || timing_.framesPerSecond <= 0.0f ||
        timing_.displaySeconds <= 0.0f) {
        return out;
    }
    if (!playing_) return out;

    const double elapsed = held_ ? heldSeconds_ : now - startSeconds_;
    // Past the end retires the panel without needing anything to have told it
    // to. Before the start only happens if a caller hands back a stamp older
    // than the one it started with, which is not a state to draw.
    if (elapsed < 0.0 || elapsed >= static_cast<double>(timing_.displaySeconds)) return out;

    const auto seconds = static_cast<float>(elapsed);
    const float fadeIn = std::clamp(seconds / kFadeInSeconds, 0.0f, 1.0f);
    const float fadeOut =
        std::clamp((timing_.displaySeconds - seconds) / kFadeOutSeconds, 0.0f, 1.0f);

    out.visible = true;
    out.alpha = std::min(fadeIn, fadeOut);
    // The authored frames loop for as long as the message is shown, so a clip
    // shorter than its own display time repeats rather than freezing on its
    // last frame. Bounded above by the elapsed check, so the multiply cannot
    // run away.
    out.frame = static_cast<int>(std::floor(seconds * timing_.framesPerSecond)) %
                timing_.frameCount;
    return out;
}

int runCommPlayerTest() {
    bool ok = true;
    const auto fail = [&ok](const std::string& why) {
        log::error("comm-player-test: {}", why);
        ok = false;
    };

    // Hard-coded rather than read from the manifest: this is a test of the
    // clock, and --comm-test checks the shipped clip separately.
    const CommClipTiming timing{16, 8.0f, 4.5f};
    const auto expectedFrame = [&](double t) {
        return static_cast<int>(std::floor(static_cast<float>(t) * timing.framesPerSecond)) %
               timing.frameCount;
    };

    CommPlayer player;
    player.configure(timing);
    if (player.at(0.0).visible) fail("a player that was never started drew something");

    // An epoch deliberately unlike zero, so a player that quietly treats `now`
    // as elapsed fails here rather than passing by coincidence.
    constexpr double kEpoch = 1234.5;
    player.start(kEpoch);
    const CommFrame opening = player.at(kEpoch);
    if (!opening.visible || opening.frame != 0) fail("the transmission did not open on frame 0");
    if (opening.alpha > 0.01f) fail("the transmission opened without fading in");

    const CommFrame middle = player.at(kEpoch + 1.0);
    if (!middle.visible || middle.frame != expectedFrame(1.0)) {
        fail("the frame one second in was not the one the clip's rate asks for");
    }
    if (middle.alpha < 0.999f) fail("the transmission was still fading a second in");

    // The reason the clock is an epoch. A frame that overran the loop's clamp,
    // or a spell minimized, leaves a gap no accumulated delta ever makes up.
    // Asking at a stamp is not allowed to care that the stamps before it were
    // sparse, so this must land where a step every 1/120s would have.
    player.at(kEpoch + 0.1);
    const CommFrame afterHitch = player.at(kEpoch + 3.0);
    if (!afterHitch.visible || afterHitch.frame != expectedFrame(3.0)) {
        fail("a jump in the clock did not land on the frame that interval belongs to");
    }

    // Retires itself, and stays retired however far past the end it is asked.
    if (player.at(kEpoch + static_cast<double>(timing.displaySeconds)).visible) {
        fail("the transmission outlived its own display time");
    }
    if (player.at(kEpoch + 1000.0).visible) fail("the transmission came back after it ended");

    // --comm: the same answer whatever the clock says.
    player.hold(1.0);
    const CommFrame held = player.at(kEpoch);
    const CommFrame stillHeld = player.at(kEpoch + 9999.0);
    if (!held.visible || held.frame != expectedFrame(1.0)) fail("--comm held the wrong frame");
    if (stillHeld.frame != held.frame || stillHeld.alpha != held.alpha) {
        fail("--comm drifted with the clock instead of holding");
    }

    player.stop();
    if (player.at(kEpoch + 1.0).visible) fail("a stopped transmission kept drawing");

    // A level with no clip at all must be silent about it, not divide by it.
    CommPlayer empty;
    empty.start(kEpoch);
    if (empty.at(kEpoch + 1.0).visible) fail("a player with no frames drew one anyway");

    if (!ok) return 1;
    log::info("comm-player-test: epoch clock, fade, retirement, hold and jump recovery: passed");
    return 0;
}

} // namespace dvk
