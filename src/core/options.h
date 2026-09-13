#pragma once

#include <string>

namespace dvk {

// Command-line options. Both exist so the app can be verified without a
// person at the keyboard:
//   --frames N          exit after N rendered frames
//   --screenshot PATH   write the final frame to PATH (PNG) before exiting
struct Options {
    int frames = -1;
    std::string screenshot;

    static Options parse(int argc, char** argv);
};

} // namespace dvk
