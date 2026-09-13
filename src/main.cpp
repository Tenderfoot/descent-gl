#include "core/input.h"
#include "core/log.h"
#include "core/options.h"
#include "core/time.h"
#include "core/window.h"
#include "game/state.h"
#include "render/debug.h"
#include "render/gl.h"
#include "render/renderer.h"
#include "render/screenshot.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/constants.hpp>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <thread>

namespace {

// The simulation runs at a fixed rate; rendering interpolates between the two
// most recent states. Everything gameplay-related from M1 on is integrated
// here, so the rate is a correctness property, not a preference.
constexpr double kSimHz = 120.0;
constexpr double kSimStep = 1.0 / kSimHz;
// Below this, a long stall (a breakpoint, a dragged window) would otherwise
// trigger a burst of catch-up steps.
constexpr double kMaxFrameTime = 0.25;

constexpr float kCamSpeed = 4.0f;       // units/second
constexpr float kMouseSens = 0.0022f;   // radians/pixel

void stepSim(dvk::SimState& s, const dvk::Input& input, glm::vec2 mouseDelta, float dt) {
    s.cubeAngle += dt * 0.9f;
    if (s.cubeAngle > glm::two_pi<float>()) s.cubeAngle -= glm::two_pi<float>();

    if (input.mouseCaptured()) {
        s.camYaw += mouseDelta.x * kMouseSens;
        s.camPitch -= mouseDelta.y * kMouseSens;
        constexpr float kLimit = glm::radians(85.0f);
        s.camPitch = std::clamp(s.camPitch, -kLimit, kLimit);
    }

    const glm::vec3 forward{
        std::cos(s.camPitch) * std::sin(s.camYaw),
        std::sin(s.camPitch),
        -std::cos(s.camPitch) * std::cos(s.camYaw),
    };
    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));

    glm::vec3 move(0.0f);
    if (input.down(GLFW_KEY_W)) move += forward;
    if (input.down(GLFW_KEY_S)) move -= forward;
    if (input.down(GLFW_KEY_D)) move += right;
    if (input.down(GLFW_KEY_A)) move -= right;
    if (input.down(GLFW_KEY_SPACE)) move += glm::vec3(0.0f, 1.0f, 0.0f);
    if (input.down(GLFW_KEY_LEFT_CONTROL)) move -= glm::vec3(0.0f, 1.0f, 0.0f);

    if (glm::length(move) > 0.0001f) {
        s.camPos += glm::normalize(move) * kCamSpeed * dt;
    }
}

// Scripted window events for --resize-test. Covers the two cases that break
// silently: a framebuffer size change while drawing, and iconify/restore,
// where the framebuffer is 0x0 for a while.
//
// Keyed to wall-clock seconds, not frame count: no frames are drawn while the
// window is iconified, so a frame-keyed schedule would never reach its own
// restore event and the run would hang minimized forever.
struct ScriptedEvent {
    double atSeconds;
    enum class Kind { Resize, Iconify, Restore } kind;
    int width, height;
};

constexpr ScriptedEvent kResizeScript[] = {
    {1.0, ScriptedEvent::Kind::Resize, 1024, 768},
    {2.0, ScriptedEvent::Kind::Resize, 800, 600},
    {3.0, ScriptedEvent::Kind::Iconify, 0, 0},
    {4.5, ScriptedEvent::Kind::Restore, 0, 0},
    {6.0, ScriptedEvent::Kind::Resize, 1920, 1080},
    {7.5, ScriptedEvent::Kind::Resize, 640, 360},
};
constexpr double kResizeTestSeconds = 9.0;

} // namespace

int main(int argc, char** argv) {
    const dvk::Options opts = dvk::Options::parse(argc, argv);

    dvk::Window::Desc desc;
#ifdef DVK_DEBUG
    desc.debugContext = true;
#endif
    // A frame-limited run must not be pinned to the refresh rate, except under
    // --resize-test, where the window manager needs real time to deliver
    // iconify and restore.
    desc.vsync = opts.frames < 0 || opts.resizeTest;

    dvk::Window window(desc);
    dvk::Input input;
    window.setInput(&input);

    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
        dvk::log::fatal("gladLoadGLLoader failed");
    }
#ifdef DVK_DEBUG
    dvk::gl::installDebugCallback(/*fatalOnHigh=*/true);
#else
    dvk::gl::installDebugCallback(/*fatalOnHigh=*/false);
#endif

    dvk::gl::Renderer renderer;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window.handle(), true);
    ImGui_ImplOpenGL3_Init("#version 460 core");
    // ImGui installs its own GLFW callbacks and chains to ours.
    window.setInput(&input);

    dvk::SimState prev;
    dvk::SimState cur;
    dvk::Clock clock;
    // Measured from here, not from construction: startup (window, GL, texture,
    // shader compile) runs no sim steps and would drag the average down.
    const double loopStart = clock.seconds();
    double lastTime = loopStart;
    double accumulator = 0.0;
    long long totalSteps = 0;
    int frame = 0;
    double frameMs = 0.0;
    double loopEnd = 0.0;   // set on the final frame, before any screenshot write
    bool scriptFired[std::size(kResizeScript)] = {};
    int minimizedFrames = 0;
    double minimizedSeconds = 0.0;

    while (!window.shouldClose()) {
        input.beginFrame();
        window.poll();

        if (opts.resizeTest) {
            const double t = clock.seconds() - loopStart;
            for (size_t i = 0; i < std::size(kResizeScript); ++i) {
                if (scriptFired[i] || t < kResizeScript[i].atSeconds) continue;
                scriptFired[i] = true;
                const auto& e = kResizeScript[i];
                switch (e.kind) {
                    case ScriptedEvent::Kind::Resize:
                        dvk::log::info("resize-test: set window {}x{}", e.width, e.height);
                        glfwSetWindowSize(window.handle(), e.width, e.height);
                        break;
                    case ScriptedEvent::Kind::Iconify:
                        dvk::log::info("resize-test: iconify");
                        glfwIconifyWindow(window.handle());
                        break;
                    case ScriptedEvent::Kind::Restore:
                        dvk::log::info("resize-test: restore");
                        glfwRestoreWindow(window.handle());
                        break;
                }
            }
            // Ends the run on time rather than on frame count, for the same
            // reason the schedule is time-keyed.
            if (t > kResizeTestSeconds) window.requestClose();
        }

        const double now = clock.seconds();
        double elapsed = now - lastTime;
        lastTime = now;
        frameMs = elapsed * 1000.0;
        elapsed = std::min(elapsed, kMaxFrameTime);
        accumulator += elapsed;

        if (window.minimized()) {
            // Nothing to draw and no valid framebuffer size; keep the sim from
            // banking time while the window is away.
            accumulator = 0.0;
            ++minimizedFrames;
            // Time spent minimized is deliberately dropped by the sim, so it
            // must come out of the denominator too or the reported step rate
            // understates by however long the window was away.
            minimizedSeconds += elapsed;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        if (input.pressed(GLFW_KEY_ESCAPE)) {
            input.setMouseCaptured(window.handle(), !input.mouseCaptured());
        }

        const glm::vec2 mouseDelta = input.consumeMouseDelta();
        bool consumedMouse = false;
        while (accumulator >= kSimStep) {
            prev = cur;
            // The whole frame's motion belongs to the first step; later steps
            // in the same frame have no new input to apply.
            stepSim(cur, input, consumedMouse ? glm::vec2(0.0f) : mouseDelta,
                    static_cast<float>(kSimStep));
            consumedMouse = true;
            accumulator -= kSimStep;
            ++totalSteps;
        }

        const auto alpha = static_cast<float>(accumulator / kSimStep);
        const dvk::RenderState rs = dvk::interpolate(prev, cur, alpha);

        const glm::ivec2 fb = window.framebufferSize();
        renderer.beginFrame(fb.x, fb.y);
        renderer.drawScene(rs);
        renderer.resolve();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        if (ImGui::Begin("descent-vk")) {
            ImGui::Text("%.2f ms  (%.0f fps)", frameMs, frameMs > 0.0 ? 1000.0 / frameMs : 0.0);
            ImGui::Text("sim steps: %lld  (%.0f Hz fixed)", totalSteps, kSimHz);
            ImGui::Text("interp alpha: %.3f", alpha);
            ImGui::Text("framebuffer: %d x %d @ %dx MSAA", fb.x, fb.y,
                        static_cast<int>(dvk::gl::Renderer::kSamples));
            ImGui::Separator();
            ImGui::Text("cam %.2f %.2f %.2f", rs.camPos.x, rs.camPos.y, rs.camPos.z);
            ImGui::Text("mouse captured: %s  (Escape toggles)",
                        input.mouseCaptured() ? "yes" : "no");
            ImGui::Separator();
            // Placeholder: M1 puts the flight tuning sliders here.
            ImGui::TextDisabled("tuning panel (M1)");
        }
        ImGui::End();
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        ++frame;
        const bool lastFrame = opts.frames >= 0 && frame >= opts.frames;
        if (lastFrame) {
            // Stop the clock before the screenshot: a PNG encode of the whole
            // framebuffer takes ~200ms, and counting it would understate the
            // step rate in the report below.
            loopEnd = clock.seconds();
            if (!opts.screenshot.empty()) {
                dvk::gl::writeScreenshot(opts.screenshot, fb.x, fb.y);
            }
        }

        window.swap();
        if (lastFrame) window.requestClose();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    // The step rate is the property worth checking: steps/elapsed must sit on
    // kSimHz regardless of how fast frames were drawn.
    const double elapsed =
        (loopEnd > 0.0 ? loopEnd : clock.seconds()) - loopStart - minimizedSeconds;
    dvk::log::info("exited cleanly: {} frames, {} sim steps in {:.3f}s ({:.1f} steps/s, target {:.0f})",
                   frame, totalSteps, elapsed,
                   elapsed > 0.0 ? static_cast<double>(totalSteps) / elapsed : 0.0, kSimHz);
    if (opts.resizeTest) {
        dvk::log::info("resize-test: {} frames ({:.2f}s) skipped while minimized, excluded above",
                       minimizedFrames, minimizedSeconds);
    }
    return 0;
}
