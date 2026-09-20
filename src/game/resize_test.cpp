#include "game/resize_test.h"

#include "core/log.h"
#include "core/window.h"

#include <GLFW/glfw3.h>

#include <iterator>

namespace dvk {

namespace {

struct ScriptedEvent {
    double atSeconds;
    enum class Kind { Resize, Iconify, Restore } kind;
    int width, height;
};

constexpr ScriptedEvent kScript[] = {
    {1.0, ScriptedEvent::Kind::Resize, 1024, 768},
    {2.0, ScriptedEvent::Kind::Resize, 800, 600},
    {3.0, ScriptedEvent::Kind::Iconify, 0, 0},
    {4.5, ScriptedEvent::Kind::Restore, 0, 0},
    {6.0, ScriptedEvent::Kind::Resize, 1920, 1080},
    {7.5, ScriptedEvent::Kind::Resize, 640, 360},
};

} // namespace

void ResizeTest::update(Window& window, double elapsed) {
    while (next_ < static_cast<int>(std::size(kScript)) &&
           elapsed >= kScript[static_cast<size_t>(next_)].atSeconds) {
        const ScriptedEvent& event = kScript[static_cast<size_t>(next_)];
        ++next_;
        switch (event.kind) {
            case ScriptedEvent::Kind::Resize:
                log::info("resize-test: set window {}x{}", event.width, event.height);
                glfwSetWindowSize(window.handle(), event.width, event.height);
                break;
            case ScriptedEvent::Kind::Iconify:
                log::info("resize-test: iconify");
                glfwIconifyWindow(window.handle());
                break;
            case ScriptedEvent::Kind::Restore:
                log::info("resize-test: restore");
                glfwRestoreWindow(window.handle());
                break;
        }
    }
}

} // namespace dvk
