#include "render/target.h"

#include "core/log.h"

namespace dvk::gl {

MsaaTarget::~MsaaTarget() { destroy(); }

void MsaaTarget::destroy() {
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (color_) glDeleteRenderbuffers(1, &color_);
    if (depthStencil_) glDeleteRenderbuffers(1, &depthStencil_);
    fbo_ = color_ = depthStencil_ = 0;
}

void MsaaTarget::resize(int width, int height, GLsizei samples) {
    if (width == width_ && height == height_ && samples == samples_) return;
    if (width <= 0 || height <= 0) return;

    destroy();
    width_ = width;
    height_ = height;
    samples_ = samples;

    glCreateRenderbuffers(1, &color_);
    glNamedRenderbufferStorageMultisample(color_, samples, GL_RGBA8, width, height);

    glCreateRenderbuffers(1, &depthStencil_);
    glNamedRenderbufferStorageMultisample(depthStencil_, samples, GL_DEPTH32F_STENCIL8,
                                          width, height);

    glCreateFramebuffers(1, &fbo_);
    glNamedFramebufferRenderbuffer(fbo_, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color_);
    glNamedFramebufferRenderbuffer(fbo_, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                   depthStencil_);

    const GLenum status = glCheckNamedFramebufferStatus(fbo_, GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        log::fatal("MSAA framebuffer incomplete: 0x{:X}", status);
    }
    log::info("render target {}x{} at {}x MSAA (DEPTH32F_STENCIL8)", width, height, samples);
}

void MsaaTarget::bind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);
}

void MsaaTarget::resolveToDefault() const {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, width_, height_, 0, 0, width_, height_,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

} // namespace dvk::gl
