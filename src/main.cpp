#include "core/input.h"
#include "core/log.h"
#include "core/options.h"
#include "core/window.h"

#include <GLFW/glfw3.h>

#include <thread>

int main(int argc, char** argv) {
    const dvk::Options opts = dvk::Options::parse(argc, argv);

    dvk::Window::Desc desc;
#ifdef DVK_DEBUG
    desc.debugContext = true;
#endif
    dvk::Window window(desc);
    dvk::Input input;
    window.setInput(&input);

    int frame = 0;
    while (!window.shouldClose()) {
        input.beginFrame();
        window.poll();

        if (window.minimized()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        if (input.pressed(GLFW_KEY_ESCAPE)) {
            input.setMouseCaptured(window.handle(), !input.mouseCaptured());
        }

        window.swap();
        ++frame;
        if (opts.frames >= 0 && frame >= opts.frames) window.requestClose();
    }

    dvk::log::info("exited after {} frames", frame);
    return 0;
}
