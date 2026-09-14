#pragma once

#include <string>

namespace dvk {

// Command-line options. These exist so the app can be verified without a
// person at the keyboard:
//   --frames N          exit after N rendered frames
//   --screenshot PATH   write the final frame to PATH (PNG) before exiting
//   --resize-test       drive a scripted sequence of window resizes, an
//                       iconify and a restore, exercising the framebuffer
//                       callback and render-target rebuild
//   --chase             start in the chase camera (F1 at runtime), so a
//                       screenshot can show the ship without a keypress
//   --flight-test       run the flight model headlessly and report the
//                       response times the tuning is supposed to hit, then
//                       exit. Needs no window and no GL.
//   --world-test        load the level, validate it, and report portal
//                       visibility and light reach from every segment, then
//                       exit. Needs no window and no GL.
//   --segment N         start in segment N's centre instead of at the
//                       player start, so a screenshot can be taken anywhere
//                       in the mine without flying there
struct Options {
    int frames = -1;
    std::string screenshot;
    bool resizeTest = false;
    bool chase = false;
    bool flightTest = false;
    bool worldTest = false;
    int startSegment = -1;

    static Options parse(int argc, char** argv);
};

} // namespace dvk
