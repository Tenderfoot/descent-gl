#include "core/input.h"
#include "core/log.h"
#include "core/options.h"
#include "core/time.h"
#include "core/window.h"
#include "game/bindings.h"
#include "game/level.h"
#include "game/state.h"
#include "game/visibility.h"
#include "game/world_test.h"
#include "render/debug.h"
#include "render/gl.h"
#include "render/renderer.h"
#include "render/screenshot.h"
#include "sim/flight.h"
#include "sim/flight_test.h"
#include "sim/ship.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
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

constexpr const char* kFlightConfigPath = "assets/config/flight.json";
constexpr const char* kLevelPath = "assets/levels/test01.json";

// The headlight. In a game with no ambient term this is the player's primary
// way of seeing anything at all, so it is deliberately generous: a wide enough
// cone to fly by and a range of three segments.
constexpr float kHeadlightRadius = 62.0f;
constexpr float kHeadlightCone = 34.0f;   // degrees, full angle
// Set a little ahead of the hull so the cone does not start inside the ship's
// own geometry.
constexpr float kHeadlightForward = 2.2f;

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

    // Measures the flight model and exits. Deliberately before the window
    // exists: it needs no GL and no display, so it runs anywhere.
    if (opts.flightTest) {
        return dvk::sim::runFlightTest(kFlightConfigPath);
    }
    if (opts.worldTest) {
        return dvk::runWorldTest(kLevelPath);
    }

    dvk::Window::Desc desc;
    desc.width = opts.width;
    desc.height = opts.height;
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

    dvk::Level level = dvk::loadLevel(kLevelPath);
    dvk::gl::Renderer renderer(level);
    renderer.drawVolumeWireframe = opts.wireframeVolumes;
    renderer.showStencil = opts.stencilView;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window.handle(), true);
    ImGui_ImplOpenGL3_Init("#version 460 core");
    // ImGui installs its own GLFW callbacks and chains to ours.
    window.setInput(&input);

    dvk::sim::FlightConfig flight = dvk::sim::loadFlightConfig(kFlightConfigPath);
    const dvk::sim::FlightConfig flightDefaults;

    dvk::SimState prev;
    dvk::SimState cur;
    cur.ship.position = level.start.position;
    cur.ship.orientation = level.start.orientation;
    cur.segment = level.start.segment;
    if (opts.startSegment >= 0 && opts.startSegment < level.segmentCount()) {
        cur.segment = opts.startSegment;
        cur.ship.position = level.segments[static_cast<size_t>(cur.segment)].center;
    }
    prev = cur;

    dvk::CameraMode camera =
        opts.chase ? dvk::CameraMode::Chase : dvk::CameraMode::FirstPerson;

    dvk::PortalWalk walk;
    dvk::LightFlood flood;

    dvk::Light headlight;
    headlight.type = dvk::LightType::Spot;
    headlight.radius = kHeadlightRadius;
    headlight.coneDegrees = kHeadlightCone;
    headlight.color = glm::vec3(1.0f, 0.97f, 0.9f);
    headlight.intensity = 3.2f;
    bool headlightOn = true;

    // Freezing the visible set leaves the last portal walk in place while the
    // camera keeps moving, so the culling can be inspected from outside it.
    // Without this, the one thing you cannot see is what the cull decided.
    bool freezeVisible = false;
    int lightEditorSelection = -1;
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
        if (input.pressed(GLFW_KEY_H)) headlightOn = !headlightOn;
        if (input.pressed(GLFW_KEY_F1)) {
            camera = camera == dvk::CameraMode::FirstPerson ? dvk::CameraMode::Chase
                                                            : dvk::CameraMode::FirstPerson;
        }

        const glm::vec2 mouseDelta = input.consumeMouseDelta();
        bool consumedMouse = false;
        while (accumulator >= kSimStep) {
            prev = cur;
            // The whole frame's motion belongs to the first step; later steps
            // in the same frame have no new input to apply.
            dvk::sim::ShipControls controls =
                dvk::readControls(input, consumedMouse ? glm::vec2(0.0f) : mouseDelta);
            dvk::sim::stepShip(cur.ship, controls, flight, static_cast<float>(kSimStep));
            cur.segment = dvk::trackSegment(level, cur.ship.position, cur.segment);
            consumedMouse = true;
            accumulator -= kSimStep;
            ++totalSteps;
        }

        const auto alpha = static_cast<float>(accumulator / kSimStep);
        dvk::RenderState rs = dvk::interpolate(prev, cur, alpha);
        rs.camera = camera;

        // The headlight moves with the ship, so its segment set is rebuilt
        // every frame. The flood fill reuses its scratch buffers, so this does
        // not allocate in the frame loop.
        const glm::mat3 shipBasis = glm::mat3_cast(rs.shipOrientation);
        const glm::vec3 shipForward = shipBasis * glm::vec3(0.0f, 0.0f, -1.0f);
        headlight.position = rs.shipPos + shipForward * kHeadlightForward;
        headlight.direction = shipForward;
        headlight.segment = rs.shipSegment;
        if (headlightOn) dvk::rebuildLightSegments(level, headlight, flood);

        const glm::ivec2 fb = window.framebufferSize();
        renderer.beginFrame(fb.x, fb.y);
        if (!freezeVisible) {
            walk.build(level, rs.shipSegment, renderer.viewProj(rs), rs.shipPos);
        }
        renderer.drawScene(rs, level, walk, headlightOn ? &headlight : nullptr);
        renderer.resolve();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(330, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("descent-vk")) {
            ImGui::Text("%.2f ms  (%.0f fps)", frameMs, frameMs > 0.0 ? 1000.0 / frameMs : 0.0);
            ImGui::Text("sim steps: %lld  (%.0f Hz fixed)", totalSteps, kSimHz);
            ImGui::Text("interp alpha: %.3f", alpha);
            ImGui::Text("framebuffer: %d x %d @ %dx MSAA", fb.x, fb.y,
                        static_cast<int>(dvk::gl::Renderer::kSamples));
            ImGui::Separator();
            const dvk::gl::Renderer::Stats& st = renderer.stats();
            ImGui::Text("segment %d of %d", rs.shipSegment, level.segmentCount());
            ImGui::Text("visible %d  (visited %d, portals %d, depth %d)",
                        static_cast<int>(walk.visible().size()), walk.segmentsVisited(),
                        walk.portalsCrossed(), walk.deepest());
            ImGui::Text("lights %d of %d reach view  (%d segment draws)", st.lightsDrawn,
                        st.lightsConsidered, st.lightSegmentDraws);
            if (ImGui::Checkbox("freeze visible set", &freezeVisible)) {
                // Nothing to do: the walk simply stops being rebuilt.
            }
            ImGui::SameLine();
            ImGui::Checkbox("light gizmos", &renderer.drawLightGizmos);
            ImGui::Separator();
            ImGui::Text("pos %.1f %.1f %.1f", rs.shipPos.x, rs.shipPos.y, rs.shipPos.z);
            ImGui::Text("speed %6.2f / %.0f units/s", rs.speed, flight.maxSpeed);
            ImGui::Text("turn  %6.2f / %.1f rad/s", rs.angularSpeed, flight.maxAngularSpeed);
            ImGui::Text("camera: %s  (F1 toggles)",
                        camera == dvk::CameraMode::Chase ? "chase" : "first person");
            ImGui::Text("mouse captured: %s  (Escape toggles)",
                        input.mouseCaptured() ? "yes" : "no");
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(12, 210), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(390, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("flight tuning")) {
            ImGui::TextDisabled("linear");
            ImGui::SliderFloat("forward thrust", &flight.forwardThrust, 0.0f, 200.0f, "%.1f");
            ImGui::SliderFloat("strafe thrust", &flight.strafeThrust, 0.0f, 200.0f, "%.1f");
            ImGui::SliderFloat("lift thrust", &flight.liftThrust, 0.0f, 200.0f, "%.1f");
            ImGui::SliderFloat("linear damping", &flight.linearDamping, 0.1f, 12.0f, "%.2f /s");
            ImGui::SliderFloat("max speed", &flight.maxSpeed, 1.0f, 80.0f, "%.1f");
            // The two numbers the defaults were actually tuned against: with
            // exponential damping a decay is visually finished at about three
            // time constants, and terminal speed is thrust / damping.
            ImGui::TextDisabled("  tau %.2fs, settles ~%.2fs, terminal %.1f u/s",
                                1.0f / flight.linearDamping, 3.0f / flight.linearDamping,
                                flight.forwardThrust / flight.linearDamping);

            ImGui::Separator();
            ImGui::TextDisabled("angular");
            ImGui::SliderFloat("mouse sensitivity", &flight.mouseSensitivity, 0.0f, 0.12f, "%.4f");
            ImGui::SliderFloat("roll torque", &flight.rollTorque, 0.0f, 60.0f, "%.1f");
            ImGui::SliderFloat("pitch torque", &flight.pitchTorque, 0.0f, 60.0f, "%.1f");
            ImGui::SliderFloat("yaw torque", &flight.yawTorque, 0.0f, 60.0f, "%.1f");
            ImGui::SliderFloat("angular damping", &flight.angularDamping, 0.5f, 25.0f, "%.2f /s");
            ImGui::SliderFloat("max angular speed", &flight.maxAngularSpeed, 0.5f, 12.0f, "%.2f");
            ImGui::Checkbox("invert pitch", &flight.invertPitch);
            // A mouse impulse rotates the ship by impulse / damping in total,
            // so this is the swipe length that produces a 180 degree turn.
            const float halfTurnPixels =
                flight.mouseSensitivity > 0.0f
                    ? glm::pi<float>() * flight.angularDamping / flight.mouseSensitivity
                    : 0.0f;
            ImGui::TextDisabled("  180 deg turn = %.0f px of mouse travel", halfTurnPixels);

            ImGui::Separator();
            if (ImGui::Button("Save")) dvk::sim::saveFlightConfig(kFlightConfigPath, flight);
            ImGui::SameLine();
            if (ImGui::Button("Reset to defaults")) flight = flightDefaults;
            ImGui::SameLine();
            if (ImGui::Button("Reload")) flight = dvk::sim::loadFlightConfig(kFlightConfigPath);
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(420, 12), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(430, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("shadows")) {
            const dvk::gl::Renderer::Stats& st = renderer.stats();
            ImGui::Checkbox("shadows", &renderer.shadowsEnabled);
            ImGui::SameLine();
            ImGui::Checkbox("volumes as wireframe", &renderer.drawVolumeWireframe);
            ImGui::Checkbox("stencil visualiser", &renderer.showStencil);
            ImGui::SameLine();
            ImGui::Checkbox("headlight (H)", &headlightOn);

            ImGui::Text("%d casters, %d silhouette edges, %d volume triangles",
                        st.shadowCasters, st.silhouetteEdges, st.volumeTriangles);

            ImGui::Separator();
            ImGui::TextDisabled("per light: edges / volume triangles / segments");
            ImGui::BeginChild("per light", ImVec2(0, 150), true);
            const auto& ls = renderer.lightStats();
            for (size_t i = 0; i < ls.size(); ++i) {
                const bool isHeadlight = i >= level.lights.size();
                dvk::Light& l = isHeadlight ? headlight : level.lights[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::Checkbox("##casts", &l.castsShadows);
                ImGui::SameLine();
                ImGui::Text("%-9s %4d edges  %5d tris  %2d segs",
                            isHeadlight ? "headlight" : "light", ls[i].silhouetteEdges,
                            ls[i].volumeTriangles, ls[i].segments);
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(12, 620), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(390, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("lights")) {
            if (ImGui::Button("Add here")) {
                dvk::Light added;
                added.position = rs.shipPos;
                added.segment = rs.shipSegment;
                added.radius = 30.0f;
                dvk::rebuildLightSegments(level, added);
                level.lights.push_back(added);
                lightEditorSelection = static_cast<int>(level.lights.size()) - 1;
            }
            ImGui::SameLine();
            if (ImGui::Button("Save level")) dvk::saveLevel(kLevelPath, level);
            ImGui::SameLine();
            ImGui::TextDisabled("%d lights", static_cast<int>(level.lights.size()));

            ImGui::BeginChild("light list", ImVec2(0, 110), true);
            for (int i = 0; i < static_cast<int>(level.lights.size()); ++i) {
                const dvk::Light& l = level.lights[static_cast<size_t>(i)];
                char label[96];
                std::snprintf(label, sizeof(label), "%2d  seg %-3d  r %-5.1f  %s  (%d segs)", i,
                              l.segment, l.radius,
                              l.type == dvk::LightType::Spot ? "spot " : "point",
                              static_cast<int>(l.segments.size()));
                if (ImGui::Selectable(label, lightEditorSelection == i)) lightEditorSelection = i;
            }
            ImGui::EndChild();

            if (lightEditorSelection >= 0 &&
                lightEditorSelection < static_cast<int>(level.lights.size())) {
                dvk::Light& l = level.lights[static_cast<size_t>(lightEditorSelection)];
                // Position and radius both change which segments the light can
                // reach, so the set is rebuilt whenever either moves. Leaving it
                // stale is how a light ends up lighting a room through a wall.
                bool reach = false;
                reach |= ImGui::DragFloat3("position", &l.position.x, 0.25f);
                reach |= ImGui::SliderFloat("radius", &l.radius, 1.0f, 120.0f, "%.1f");
                ImGui::ColorEdit3("colour", &l.color.r);
                ImGui::SliderFloat("intensity", &l.intensity, 0.0f, 12.0f, "%.2f");

                int type = l.type == dvk::LightType::Spot ? 1 : 0;
                if (ImGui::Combo("type", &type, "point\0spot\0")) {
                    l.type = type == 1 ? dvk::LightType::Spot : dvk::LightType::Point;
                }
                if (l.type == dvk::LightType::Spot) {
                    ImGui::DragFloat3("direction", &l.direction.x, 0.02f, -1.0f, 1.0f);
                    ImGui::SliderFloat("cone", &l.coneDegrees, 5.0f, 170.0f, "%.0f deg");
                }

                if (reach) {
                    l.segment = dvk::findSegment(level, l.position, l.segment);
                    dvk::rebuildLightSegments(level, l);
                }
                if (ImGui::Button("Remove")) {
                    level.lights.erase(level.lights.begin() + lightEditorSelection);
                    lightEditorSelection = -1;
                }
            } else {
                ImGui::TextDisabled("select a light to edit it");
            }
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
