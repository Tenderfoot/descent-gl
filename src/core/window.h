#pragma once

#include <glm/vec2.hpp>

struct GLFWwindow;

namespace dvk {

class Input;

// Owns the GLFW window and the OpenGL 4.6 core context.
// Rendering code loads GL function pointers separately (render/gl.h) so that
// core/ has no dependency on glad.
class Window {
public:
    struct Desc {
        int width = 1600;
        int height = 900;
        const char* title = "descent";
        bool debugContext = false;   // GL_CONTEXT_FLAG_DEBUG_BIT: enables KHR_debug output
        bool vsync = true;
    };

    explicit Window(const Desc& desc);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool shouldClose() const;
    void requestClose();
    void poll();
    void swap();

    glm::ivec2 framebufferSize() const { return fbSize_; }
    glm::ivec2 windowSize() const;
    bool minimized() const { return fbSize_.x == 0 || fbSize_.y == 0; }
    bool consumeResized();   // true once after any size change

    void setInput(Input* input) { input_ = input; }
    GLFWwindow* handle() const { return win_; }

private:
    static void onFramebufferSize(GLFWwindow* w, int width, int height);
    static void onKey(GLFWwindow* w, int key, int scancode, int action, int mods);
    static void onCursor(GLFWwindow* w, double x, double y);
    static void onMouseButton(GLFWwindow* w, int button, int action, int mods);

    GLFWwindow* win_ = nullptr;
    Input* input_ = nullptr;
    glm::ivec2 fbSize_{0, 0};
    bool resized_ = false;
};

} // namespace dvk
