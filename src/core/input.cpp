#include "core/input.h"

#include <GLFW/glfw3.h>

namespace dvk {

void Input::beginFrame() {
    prev_ = cur_;
}

bool Input::down(int key) const {
    return key >= 0 && key < kMaxKeys && cur_[static_cast<size_t>(key)];
}

bool Input::pressed(int key) const {
    if (key < 0 || key >= kMaxKeys) return false;
    const auto k = static_cast<size_t>(key);
    return cur_[k] && !prev_[k];
}

glm::vec2 Input::consumeMouseDelta() {
    const glm::vec2 d = delta_;
    delta_ = {0.0f, 0.0f};
    return d;
}

void Input::setMouseCaptured(GLFWwindow* w, bool captured) {
    captured_ = captured;
    glfwSetInputMode(w, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (glfwRawMouseMotionSupported()) {
        glfwSetInputMode(w, GLFW_RAW_MOUSE_MOTION, captured ? GLFW_TRUE : GLFW_FALSE);
    }
    // Reset the reference point so the jump to/from the hidden cursor is not
    // read as motion.
    haveLastCursor_ = false;
    delta_ = {0.0f, 0.0f};
}

void Input::onKey(int key, int action) {
    if (key < 0 || key >= kMaxKeys) return;
    if (action == GLFW_PRESS) cur_[static_cast<size_t>(key)] = true;
    else if (action == GLFW_RELEASE) cur_[static_cast<size_t>(key)] = false;
}

void Input::onCursor(double x, double y) {
    if (!captured_) {
        haveLastCursor_ = false;
        return;
    }
    if (haveLastCursor_) {
        delta_.x += static_cast<float>(x - lastCursor_.x);
        delta_.y += static_cast<float>(y - lastCursor_.y);
    }
    lastCursor_ = {x, y};
    haveLastCursor_ = true;
}

} // namespace dvk
