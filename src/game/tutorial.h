#pragma once

#include <string_view>

namespace dvk {

struct ScriptTutorial;

namespace audio {
class Audio;
}

namespace gl {
class Renderer;
}

// What the pane should draw this frame. `frame` indexes the clip the pane is
// showing and means nothing when the pane is down.
struct TutorialFrame {
    int frame = 0;
    float alpha = 0.0f;
};

// A tutorial pane's clip and the clock it loops on.
//
// Deliberately not a CommPlayer. That class exists to retire a transmission on
// time: it fades out and reports itself invisible once the message's display
// seconds are up, because a transmission is something the player flies past.
// Nothing about a tutorial ends on a clock. It loops until the player says
// they have read it, so there is no display time to hold, no fade out to run,
// and no state in which it is playing but not visible.
//
// What it does share with CommPlayer, and for the same reason, is an absolute
// epoch rather than an accumulated delta - see comm_player.h. It is the
// application clock, not the simulation's: the simulation is stopped for as
// long as this is up, so a pane driven by the fixed step would sit on one
// frame of its own video.
//
// The renderer owns the clip library and the audio system owns the voice, so
// both must outlive the pane.
class TutorialPane {
public:
    TutorialPane(const gl::Renderer& renderer, audio::Audio& audio);

    // Whether the library has a clip of that name. Answers rather than
    // decides, exactly as CommDirector::knows does: at load an unknown name is
    // fatal, and that is the caller's call because the caller knows whose name
    // it is.
    bool knows(std::string_view name) const;

    // Raises the pane and starts the clip's audio, which plays once. Nothing
    // here pauses the game or shows the cursor - that belongs to the shell,
    // which owns the session and the window.
    void show(const ScriptTutorial& tutorial, double now);

    // The player acknowledged it. Stops the audio, because the pane going away
    // mid-sentence with the voice still talking sounds like a bug.
    void dismiss();

    // Leaving or restarting the level. The same thing as dismiss() today, and
    // named apart from it because they are different events: one is the player
    // finishing with a pane and the other is a pane that should never have
    // outlived the run it belonged to.
    void clear();

    bool active() const { return active_; }
    // Which clip the pane is drawing. Meaningless while it is down.
    int clip() const { return clip_; }

    // `now` is the application clock, the same one the main loop reads. The
    // frames loop for as long as the pane is up; the alpha fades in once and
    // then stays, because there is no end to fade towards.
    TutorialFrame frame(double now) const;

private:
    const gl::Renderer* renderer_ = nullptr;
    audio::Audio* audio_ = nullptr;
    bool active_ = false;
    int clip_ = 0;
    int frameCount_ = 0;
    float framesPerSecond_ = 0.0f;
    double startSeconds_ = 0.0;
};

} // namespace dvk
