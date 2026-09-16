#pragma once

namespace dvk {

// Timing of the authored comm clip, as loaded. Enough to decide what to show
// and when to stop; the pixels themselves stay with whoever uploaded them.
struct CommClipTiming {
    int frameCount = 0;
    float framesPerSecond = 0.0f;
    float displaySeconds = 0.0f;
};

// What the HUD should draw this frame. `frame` indexes the authored sequence
// and means nothing when `visible` is false.
struct CommFrame {
    bool visible = false;
    int frame = 0;
    float alpha = 0.0f;
};

// A transmission's clock and lifetime.
//
// This used to be sim::CommState, advanced by stepGame. Nothing in the
// simulation ever read it back - it was a playback cursor living inside the
// thing the simulation is, and it forced the panel onto the fixed-step clock.
// That is the wrong clock for a video: the sound beside it will run on the
// audio device's, and the two only stay together if neither is tied to how the
// simulation happened to be stepping.
//
// The clock here is an absolute epoch, not an accumulated delta, and that is
// the whole point. The main loop's per-frame delta is deliberately clamped to
// kMaxFrameTime and dropped entirely while the window is minimized, so anything
// that adds those deltas up falls behind the wall clock by however long the
// frame hitched and never catches up. Subtracting a start stamp from the
// current time cannot drift, whatever happened in between.
//
// Nothing here pauses. A transmission keeps running behind the pause menu,
// which is a decision and not an oversight: the mixer has no pause and no way
// to address a voice once it has started, so pausing only the picture would
// leave the voice talking over a frozen frame. The HUD is drawn while paused
// anyway.
class CommPlayer {
public:
    void configure(const CommClipTiming& timing) { timing_ = timing; }
    const CommClipTiming& timing() const { return timing_; }

    // `now` comes from the application clock - the same one the main loop
    // reads, not the simulation's accumulated time.
    void start(double now);
    void stop();

    // Pins playback to one point in the clip regardless of the clock, so
    // --comm can put a repeatable frame in a screenshot. Determinism comes
    // from the flag, the way it always did, rather than from the clock.
    void hold(double seconds);

    CommFrame at(double now) const;
    bool playing() const { return playing_; }

private:
    CommClipTiming timing_;
    double startSeconds_ = 0.0;
    double heldSeconds_ = 0.0;
    bool playing_ = false;
    bool held_ = false;
};

// Drives the player's clock headlessly: that it starts, retires itself, pins
// under --comm, and above all that a jump in the clock lands exactly where the
// same interval taken in small steps would. Needs no window, no GL and no clip.
int runCommPlayerTest();

} // namespace dvk
