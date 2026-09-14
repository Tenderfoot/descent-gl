#include "core/options.h"

#include "core/log.h"

#include <algorithm>
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
        } else if (a == "--automap") {
            o.automap = true;
        } else if (a == "--validate-assets") {
            o.validateAssets = true;
        } else if (a == "--reveal") {
            o.reveal = true;
            o.automap = true;
        } else if (a == "--chase") {
            o.chase = true;
        } else if (a == "--flight-test") {
            o.flightTest = true;
        } else if (a == "--world-test") {
            o.worldTest = true;
        } else if (a == "--collision-test") {
            o.collisionTest = true;
        } else if (a == "--weapons-test") {
            o.weaponsTest = true;
        } else if (a == "--robots-test") {
            o.robotsTest = true;
        } else if (a == "--loop-test") {
            o.loopTest = true;
        } else if (a == "--light-test") {
            o.lightTest = true;
        } else if (a == "--headlight") {
            o.headlight = true;
        } else if (a == "--sound-test") {
            o.soundTest = true;
        } else if (a == "--no-sound") {
            o.noSound = true;
        } else if (a == "--volume" && hasNext) {
            o.volume = std::clamp(static_cast<float>(std::atof(argv[++i])), 0.0f, 1.0f);
        } else if (a == "--segment" && hasNext) {
            o.startSegment = std::atoi(argv[++i]);
        } else if (a == "--wireframe-volumes") {
            o.wireframeVolumes = true;
        } else if (a == "--stencil-view") {
            o.stencilView = true;
        } else if (a == "--level" && hasNext) {
            o.level = argv[++i];
        } else if (a == "--no-ui") {
            o.noUi = true;
        } else if (a == "--debug-ui") {
            o.debugUi = true;
        } else if (a == "--start-menu") {
            o.startMenu = true;
        } else if (a == "--autofire" && hasNext) {
            o.autofire = std::atoi(argv[++i]);
        } else if (a == "--debug-lines") {
            o.debugLines = true;
        } else if (a == "--size" && i + 2 < argc) {
            o.width = std::atoi(argv[++i]);
            o.height = std::atoi(argv[++i]);
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
