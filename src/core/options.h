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
struct Options {
    int frames = -1;
    std::string screenshot;
    bool resizeTest = false;

    static Options parse(int argc, char** argv);
};

} // namespace dvk
