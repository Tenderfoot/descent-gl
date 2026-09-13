#pragma once

#include "render/gl.h"

namespace dvk::gl {

// The offscreen render target everything is drawn into: a multisampled colour
// buffer plus a multisampled DEPTH32F_STENCIL8 buffer, resolved to the default
// framebuffer with a blit at the end of the frame.
//
// The stencil aspect is unused in M0 but must exist and be multisampled:
// M3 counts shadow volumes in it, and multisampled stencil is what gives the
// hard shadow edges their anti-aliasing.
class MsaaTarget {
public:
    MsaaTarget() = default;
    ~MsaaTarget();
    MsaaTarget(const MsaaTarget&) = delete;
    MsaaTarget& operator=(const MsaaTarget&) = delete;

    // Recreates the attachments. Safe to call every frame; only acts on change.
    void resize(int width, int height, GLsizei samples);

    void bind() const;
    void resolveToDefault() const;

    int width() const { return width_; }
    int height() const { return height_; }
    GLsizei samples() const { return samples_; }

private:
    void destroy();

    GLuint fbo_ = 0, color_ = 0, depthStencil_ = 0;
    int width_ = 0, height_ = 0;
    GLsizei samples_ = 0;
};

} // namespace dvk::gl
