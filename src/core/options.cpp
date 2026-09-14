#include "core/options.h"

#include "core/log.h"

#include <cstdlib>
#include <string_view>

namespace dvk {

Options Options::parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        const bool hasNext = i + 1 < argc;
        if (a == "--frames" && hasNext) {
            o.frames = std::atoi(argv[++i]);
        } else if (a == "--screenshot" && hasNext) {
            o.screenshot = argv[++i];
        } else if (a == "--resize-test") {
            o.resizeTest = true;
        } else if (a == "--chase") {
            o.chase = true;
        } else if (a == "--flight-test") {
            o.flightTest = true;
        } else {
            log::warn("ignoring unknown argument '{}'", a);
        }
    }
    if (!o.screenshot.empty() && o.frames < 0) {
        // A screenshot needs a definite last frame.
        o.frames = 60;
    }
    return o;
}

} // namespace dvk
