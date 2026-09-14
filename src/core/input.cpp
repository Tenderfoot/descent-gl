#include "core/input.h"

#include <GLFW/glfw3.h>

namespace dvk {

void Input::beginFrame() {
    prev_ = cur_;
    buttonPrev_ = buttonCur_;
}

bool Input::down(int key) const {
    return key >= 0 && key < kMaxKeys && cur_[static_cast<size_t>(key)];
}

bool Input::pressed(int key) const {
    if (key < 0 || key >= kMaxKeys) return false;
    const auto k = static_cast<size_t>(key);
    return cur_[k] && !prev_[k];
}

bool Input::anyKeyPressed() const {
    for (size_t i = 0; i < kMaxKeys; ++i) {
        if (cur_[i] && !prev_[i]) return true;
    }
    return false;
}

bool Input::mouseDown(int button) const {
    return button >= 0 && button < kMaxButtons && buttonCur_[static_cast<size_t>(button)];
}

bool Input::mousePressed(int button) const {
    if (button < 0 || button >= kMaxButtons) return false;
    const auto b = static_cast<size_t>(button);
    return buttonCur_[b] && !buttonPrev_[b];
}

void Input::onMouseButton(int button, int action) {
    if (button < 0 || button >= kMaxButtons) return;
    if (action == GLFW_PRESS) buttonCur_[static_cast<size_t>(button)] = true;
    if (action == GLFW_RELEASE) buttonCur_[static_cast<size_t>(button)] = false;
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
