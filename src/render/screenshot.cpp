#include "render/screenshot.h"

#include "core/log.h"
#include "render/gl.h"

#include <stb_image_write.h>

#include <vector>

namespace dvk::gl {

bool writeScreenshot(const std::string& path, int width, int height) {
    if (width <= 0 || height <= 0) return false;

    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    // GL's origin is bottom-left; PNG's is top-left.
    stbi_flip_vertically_on_write(1);
    const int ok = stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4);
    if (!ok) {
        log::error("failed to write screenshot '{}'", path);
        return false;
    }
    log::info("wrote screenshot '{}' ({}x{})", path, width, height);
    return true;
}

} // namespace dvk::gl
