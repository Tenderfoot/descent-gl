#pragma once

#include <string>

namespace dvk::gl {

// Reads the default framebuffer and writes a PNG. Used by --screenshot so a
// build can be verified without a person looking at the window.
bool writeScreenshot(const std::string& path, int width, int height);

} // namespace dvk::gl
