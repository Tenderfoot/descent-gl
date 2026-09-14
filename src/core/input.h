#pragma once

#include <array>
#include <glm/vec2.hpp>

struct GLFWwindow;

namespace dvk {

// Keyboard and mouse state. Fed by Window's GLFW callbacks.
//
// Key state is sampled per frame (beginFrame copies current -> previous so
// pressed() detects edges). Mouse motion accumulates until the simulation
// consumes it, so a frame with zero sim steps does not drop the motion.
class Input {
public:
    void beginFrame();

    bool down(int glfwKey) const;
    bool pressed(int glfwKey) const;   // went down this frame

    // Mouse buttons. Held state for the guns, which fire while the button is
    // down; edge state for anything that should happen once per click.
    bool mouseDown(int glfwButton) const;
    bool mousePressed(int glfwButton) const;

    // Any key at all went down this frame, for "press any key to continue".
    bool anyKeyPressed() const;

    glm::vec2 consumeMouseDelta();     // pixels moved since last consume
    bool mouseCaptured() const { return captured_; }
    void setMouseCaptured(GLFWwindow* w, bool captured);

    // Called by Window.
    void onKey(int key, int action);
    void onCursor(double x, double y);
    void onMouseButton(int button, int action);

private:
    static constexpr int kMaxKeys = 512;
    static constexpr int kMaxButtons = 8;
    std::array<bool, kMaxKeys> cur_{};
    std::array<bool, kMaxKeys> prev_{};
    std::array<bool, kMaxButtons> buttonCur_{};
    std::array<bool, kMaxButtons> buttonPrev_{};
    glm::dvec2 lastCursor_{0.0, 0.0};
    bool haveLastCursor_ = false;
    glm::vec2 delta_{0.0f, 0.0f};
    bool captured_ = false;
};

} // namespace dvk
