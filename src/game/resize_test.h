#pragma once

namespace dvk {

class Window;

// --resize-test: a scripted sequence of window resizes, an iconify and a
// restore. It covers the two cases that break silently - a framebuffer size
// change while drawing, and iconify/restore, where the framebuffer is 0x0 for a
// while - neither of which any headless check can reach.
//
// The schedule is keyed to wall-clock seconds and not to frame count: no frames
// are drawn while the window is iconified, so a frame-keyed schedule would
// never reach its own restore event and the run would hang minimized forever.
class ResizeTest {
public:
    static constexpr double kSeconds = 9.0;

    // Fires whatever the schedule has due by `elapsed` seconds into the run.
    void update(Window& window, double elapsed);

    // Ends the run on time, for the same reason the schedule is time-keyed.
    static bool finished(double elapsed) { return elapsed > kSeconds; }

private:
    // Which event comes next. The schedule is in ascending time order, so a
    // cursor is enough and a frame that swallowed several of them fires all.
    int next_ = 0;
};

} // namespace dvk
