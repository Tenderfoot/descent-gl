#include "core/window.h"

#include "core/input.h"
#include "core/log.h"

#include <GLFW/glfw3.h>

namespace dvk {

namespace {
void onGlfwError(int code, const char* desc) {
    log::error("GLFW error {}: {}", code, desc);
}
} // namespace

Window::Window(const Desc& desc) {
    glfwSetErrorCallback(onGlfwError);
    if (!glfwInit()) log::fatal("glfwInit failed");

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, desc.debugContext ? GLFW_TRUE : GLFW_FALSE);
    // The default framebuffer is only a blit target: our own MSAA framebuffer
    // carries colour, depth and stencil.
    glfwWindowHint(GLFW_SAMPLES, 0);
    glfwWindowHint(GLFW_DEPTH_BITS, 0);
    glfwWindowHint(GLFW_STENCIL_BITS, 0);
    glfwWindowHint(GLFW_SRGB_CAPABLE, GLFW_FALSE);

    win_ = glfwCreateWindow(desc.width, desc.height, desc.title, nullptr, nullptr);
    if (!win_) log::fatal("glfwCreateWindow failed (is an OpenGL 4.6 core driver available?)");

    glfwSetWindowUserPointer(win_, this);
    glfwSetFramebufferSizeCallback(win_, onFramebufferSize);
    glfwSetKeyCallback(win_, onKey);
    glfwSetCursorPosCallback(win_, onCursor);
    glfwSetMouseButtonCallback(win_, onMouseButton);

    glfwMakeContextCurrent(win_);
    glfwSwapInterval(desc.vsync ? 1 : 0);

    glfwGetFramebufferSize(win_, &fbSize_.x, &fbSize_.y);
    resized_ = true;
}

Window::~Window() {
    if (win_) glfwDestroyWindow(win_);
    glfwTerminate();
}

bool Window::shouldClose() const { return glfwWindowShouldClose(win_); }
void Window::requestClose() { glfwSetWindowShouldClose(win_, GLFW_TRUE); }
void Window::poll() { glfwPollEvents(); }
void Window::swap() { glfwSwapBuffers(win_); }

glm::ivec2 Window::windowSize() const {
    glm::ivec2 size;
    glfwGetWindowSize(win_, &size.x, &size.y);
    return size;
}

bool Window::consumeResized() {
    const bool r = resized_;
    resized_ = false;
    return r;
}

void Window::onFramebufferSize(GLFWwindow* w, int width, int height) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
    self->fbSize_ = {width, height};
    self->resized_ = true;
}

void Window::onKey(GLFWwindow* w, int key, int /*scancode*/, int action, int /*mods*/) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
    if (self->input_) self->input_->onKey(key, action);
}

void Window::onMouseButton(GLFWwindow* w, int button, int action, int) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
    if (self && self->input_) self->input_->onMouseButton(button, action);
}

void Window::onCursor(GLFWwindow* w, double x, double y) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
    if (self->input_) self->input_->onCursor(x, y);
}

} // namespace dvk
