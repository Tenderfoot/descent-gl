#include "audio/audio.h"
#include "audio/sound_test.h"
#include "core/input.h"
#include "core/log.h"
#include "core/options.h"
#include "core/time.h"
#include "core/window.h"
#include "game/asset_check.h"
#include "game/bindings.h"
#include "game/level.h"
#include "game/state.h"
#include "game/visibility.h"
#include "game/collision_test.h"
#include "game/light_test.h"
#include "game/loop_test.h"
#include "game/robots_test.h"
#include "game/session.h"
#include "game/weapons_test.h"
#include "game/world_test.h"
#include "render/debug.h"
#include "render/gl.h"
#include "core/alloc_count.h"
#include "render/renderer.h"
#include "render/screenshot.h"
#include "sim/collision.h"
#include "sim/combat.h"
#include "sim/flight.h"
#include "sim/flight_test.h"
#include "sim/gameflow.h"
#include "sim/lighting.h"
#include "sim/robots.h"
#include "sim/ship.h"
#include "sim/weapons.h"

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
#include <string>
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
constexpr const char* kDefaultLevelPath = "assets/levels/mine01.json";
constexpr const char* kWeaponConfigPath = "assets/config/weapons.json";
constexpr const char* kRobotConfigPath = "assets/config/robots.json";
constexpr const char* kSoundDir = "assets/sounds";

// How the ship's drive is heard.
//
// Silent at rest. There is no idle hum: a parked ship droning away is one more
// sound competing with the mine for the player's attention, and the drive is
// meant to be information about motion. Below kEngineSilentBelow the channel is
// stopped outright rather than played quietly.
//
// The gain is what the drive reaches at top speed, and it is half what M9
// shipped - at 0.55 the engine sat on top of the guns rather than under them.
constexpr float kEngineSilentBelow = 0.5f;   // units/s, out of a top speed of 20
constexpr float kEngineGain = 0.275f;
constexpr float kEngineLowPitch = 0.72f;
constexpr float kEngineHighPitch = 1.28f;

// The headlight. In a game with no ambient term this is the player's primary
// way of seeing anything at all, so it is deliberately generous: a wide enough
// cone to fly by and a range of three segments.
constexpr float kHeadlightRadius = 62.0f;
constexpr float kHeadlightCone = 60.0f;   // degrees, full angle
// Wide and soft rather than narrow and hot. A tight bright beam turned the
// mine into a spotlight on a wall with nothing either side of it; widening the
// cone and pulling the intensity down trades the hotspot for a pool of light
// that shows where the corridor goes. It also leaves the level's own lamps as
// the brightest things in the mine, which is what makes flying towards one
// feel like progress.
constexpr float kHeadlightIntensity = 2.0f;
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
    const char* levelPath = opts.level.empty() ? kDefaultLevelPath : opts.level.c_str();

    // Measures the flight model and exits. Deliberately before the window
    // exists: it needs no GL and no display, so it runs anywhere.
    if (opts.flightTest) {
        return dvk::sim::runFlightTest(kFlightConfigPath);
    }
    // Every shipped asset through the validators, then out. Needs no window and
    // no GL, so it runs anywhere the tests do.
    if (opts.validateAssets) {
        return dvk::validateAssets() == 0 ? 0 : 1;
    }
    if (opts.worldTest) {
        return dvk::runWorldTest(levelPath);
    }
    if (opts.collisionTest) {
        return dvk::runCollisionTest(levelPath, kFlightConfigPath);
    }
    if (opts.weaponsTest) {
        return dvk::runWeaponsTest(levelPath, kWeaponConfigPath, kRobotConfigPath);
    }
    if (opts.robotsTest) {
        return dvk::runRobotsTest(levelPath, kWeaponConfigPath, kRobotConfigPath);
    }
    if (opts.loopTest) {
        return dvk::runLoopTest(levelPath, kWeaponConfigPath, kRobotConfigPath);
    }
    if (opts.lightTest) {
        return dvk::runLightTest(levelPath);
    }
    // The whole audio path with no device and no sound card. Sound is the one
    // subsystem a screenshot cannot show, so this is how a claim about it is
    // made checkable.
    if (opts.soundTest) {
        return dvk::audio::runSoundTest(kSoundDir);
    }

    // Every shipped asset, every run, before a window exists. A broken asset
    // then fails before anything is on screen rather than at the moment the
    // level that places it is first loaded.
    dvk::validateAssets();

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

    dvk::Level level = dvk::loadLevel(levelPath);
    const dvk::sim::RobotConfig robotConfig = dvk::sim::loadRobotConfig(kRobotConfigPath);
    dvk::gl::Renderer renderer(level, robotConfig);
    renderer.drawVolumeWireframe = opts.wireframeVolumes;
    renderer.showStencil = opts.stencilView;
    renderer.drawWallNormals = opts.debugLines;
    renderer.drawNoseRay = opts.debugLines;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window.handle(), true);
    ImGui_ImplOpenGL3_Init("#version 460 core");
    // ImGui installs its own GLFW callbacks and chains to ours.
    window.setInput(&input);

    dvk::sim::FlightConfig flight = dvk::sim::loadFlightConfig(kFlightConfigPath);
    const dvk::sim::FlightConfig flightDefaults;

    dvk::LevelSession session;
    session.load(level, robotConfig, opts.startSegment);
    dvk::SimState& prev = session.previous();
    dvk::SimState& cur = session.current();
    // The headlight is off at the start, so a frame-limited run has no way to
    // show it without someone pressing H. Both states have to be screenshot-
    // able for the retune to be checkable at all. Set on the initial state
    // rather than the current one: start() and every restart copy that over
    // the live state, so anything set only on current() lasts until the run
    // actually begins and no longer.
    if (opts.headlight) {
        session.initial().combat.player.headlightOn = true;
        prev.combat.player.headlightOn = true;
        cur.combat.player.headlightOn = true;
    }
    dvk::PlayerMenu menu;

    const bool commandLineRun = argc > 1 && !opts.startMenu;
    if (commandLineRun) {
        session.start();
        menu.hide();
    } else {
        menu.showMain();
    }

    dvk::sim::ContactLog contacts;

    // Sound. A container with no sound card is the ordinary case for this and
    // is a warning, not an error: the mixer still runs, nothing pulls from it,
    // and every other number the run reports is unaffected.
    dvk::audio::Audio audio;
    if (!opts.noSound) {
        audio.init(kSoundDir);
        audio.setMasterGain(opts.volume);
    }
    // Filled by the simulation steps of one frame and drained once, after them.
    // Never cleared inside the step loop: a frame that runs three steps must
    // still hear all three.
    dvk::sim::EventQueue soundEvents;

    dvk::CameraMode camera =
        opts.chase ? dvk::CameraMode::Chase : dvk::CameraMode::FirstPerson;

    // F3 brings the debug panels back. Off by default, because from M7 the HUD
    // is what the player reads and the panels are for tuning.
    bool showDebugUi = !opts.noUi && opts.debugUi;
    dvk::gl::AutomapData automap;
    automap.open = opts.automap;

    dvk::PortalWalk walk;
    dvk::LightFlood flood;

    dvk::sim::WeaponConfig weapons = dvk::sim::loadWeaponConfig(kWeaponConfigPath);

    const dvk::sim::GameConfig gameConfig;

    // Rebuilt each step and handed to collision, line of sight and projectiles
    // alike, so none of them can disagree about whether a door is shut.
    dvk::sim::SolidDoors solidDoors;
    // The player's own illumination, recomputed each frame and read by the
    // debug panel. Held out here so it survives between frames rather than
    // being rebuilt inside the UI block.
    dvk::sim::Illumination playerLight;

    // The level's own lights had their segment sets computed at load with no
    // doors. They are recomputed against the door state now, and again every
    // time a door changes between see-through and shut, so a light beside a
    // shut door stops at it the way a bolt light does.
    int lightDoorSignature = -1;

    // Rebuilt every frame and reused, so the vectors inside each Light keep
    // their capacity and the draw path does not allocate. Index 0 is always
    // the headlight, which is the convention the renderer relies on to know
    // which light the ship must not shadow.
    std::vector<dvk::Light> dynamicLights;
    dynamicLights.reserve(1 + dvk::sim::kMaxProjectiles + dvk::sim::kMaxFlares +
                          dvk::sim::kMaxEffects);
    int lightEditorSelection2 = -1;
    (void)lightEditorSelection2;

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
    // Allocation instrumentation. The first frames are exempt: pools and
    // vectors reserve on their way to steady state, and counting that warm-up
    // would report a leak that is really a start-up cost.
    constexpr int kAllocWarmupFrames = 30;
    uint64_t worstSimAllocs = 0;
    uint64_t worstDrawAllocs = 0;
    int simAllocFrames = 0;
    int drawAllocFrames = 0;
    int firstSimAllocFrame = -1;
    int firstDrawAllocFrame = -1;
    double loopEnd = 0.0;   // set on the final frame, before any screenshot write
    bool scriptFired[std::size(kResizeScript)] = {};
    int minimizedFrames = 0;
    double minimizedSeconds = 0.0;
    double suspendedSeconds = 0.0;

    if (session.advancesSimulation() && opts.frames < 0 && !opts.resizeTest) {
        input.setMouseCaptured(window.handle(), true);
    }

    while (!window.shouldClose()) {
        const bool simulationWasAdvancing = session.advancesSimulation();
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

        if (!window.minimized() && !simulationWasAdvancing) suspendedSeconds += elapsed;

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

        if (input.pressed(dvk::keyFor(dvk::Toggle::Pause))) {
            if (session.mode() == dvk::AppMode::Playing) {
                session.pause();
                menu.showPause();
                automap.open = false;
                input.setMouseCaptured(window.handle(), false);
            } else if (session.mode() == dvk::AppMode::Paused) {
                session.resume();
                menu.hide();
                input.setMouseCaptured(window.handle(), true);
            }
            accumulator = 0.0;
            prev = cur;
        }

        const glm::ivec2 menuFramebuffer = window.framebufferSize();
        const glm::ivec2 menuWindow = window.windowSize();
        const glm::vec2 pointerScale(
            static_cast<float>(menuFramebuffer.x) / static_cast<float>(std::max(menuWindow.x, 1)),
            static_cast<float>(menuFramebuffer.y) / static_cast<float>(std::max(menuWindow.y, 1)));
        const dvk::MenuAction menuAction = menu.update(
            dvk::readMenuInput(input, input.cursorPosition() * pointerScale), menuFramebuffer.x,
            menuFramebuffer.y);
        switch (menuAction) {
            case dvk::MenuAction::Start:
                session.start();
                menu.hide();
                lightDoorSignature = -1;
                input.setMouseCaptured(window.handle(), true);
                accumulator = 0.0;
                break;
            case dvk::MenuAction::Resume:
                session.resume();
                menu.hide();
                input.setMouseCaptured(window.handle(), true);
                accumulator = 0.0;
                prev = cur;
                break;
            case dvk::MenuAction::Restart:
                session.restart();
                menu.hide();
                contacts.clear();
                automap.open = false;
                lightDoorSignature = -1;
                input.setMouseCaptured(window.handle(), true);
                accumulator = 0.0;
                break;
            case dvk::MenuAction::MainMenu:
                session.leave();
                menu.showMain();
                contacts.clear();
                automap.open = false;
                input.setMouseCaptured(window.handle(), false);
                accumulator = 0.0;
                prev = cur;
                break;
            case dvk::MenuAction::Quit: window.requestClose(); break;
            case dvk::MenuAction::None: break;
        }

        if (input.pressed(dvk::keyFor(dvk::Toggle::DebugUi))) {
            showDebugUi = !showDebugUi;
        } else if (session.mode() == dvk::AppMode::Playing &&
                   input.pressed(dvk::keyFor(dvk::Toggle::Headlight))) {
            // Refuses to come on with no energy left, which is the whole point
            // of it costing any.
            cur.combat.player.headlightOn =
                !cur.combat.player.headlightOn && cur.combat.player.energy > 0.0f;
        }
        if (input.pressed(dvk::keyFor(dvk::Toggle::Chase))) {
            camera = camera == dvk::CameraMode::FirstPerson ? dvk::CameraMode::Chase
                                                            : dvk::CameraMode::FirstPerson;
        }
        if (session.mode() == dvk::AppMode::Playing &&
            input.pressed(dvk::keyFor(dvk::Toggle::Automap))) {
            automap.open = !automap.open;
        }
        if (input.pressed(dvk::keyFor(dvk::Toggle::Mute))) {
            // Mute keeps the device open and the ambient channels running, so
            // unmuting is instant and does not restart the engine mid-note.
            audio.setMuted(!audio.muted());
        }

        if (!session.advancesSimulation()) accumulator = 0.0;

        dvk::alloc::Scope simAllocs;
        glm::vec2 mouseDelta = input.consumeMouseDelta();
        // With the map open the mouse orbits it instead of steering. The
        // simulation is untouched either way: the ship keeps flying, robots keep
        // hunting, and the countdown keeps running - opening the map is not a
        // pause, and reading it while something is shooting at you is the cost
        // of looking.
        if (automap.open) {
            automap.yaw += mouseDelta.x * 0.005f;
            automap.pitch = std::clamp(automap.pitch - mouseDelta.y * 0.005f, -1.45f, 1.45f);
            mouseDelta = glm::vec2(0.0f);
        }
        bool consumedMouse = false;
        soundEvents.clear();
        while (session.advancesSimulation() && accumulator >= kSimStep) {
            prev = cur;
            // The whole frame's motion belongs to the first step; later steps
            // in the same frame have no new input to apply.
            dvk::sim::ShipControls controls =
                dvk::readControls(input, consumedMouse ? glm::vec2(0.0f) : mouseDelta);
            dvk::sim::stepShip(cur.ship, controls, flight, static_cast<float>(kSimStep));

            // Collision runs after integration, every step, and owns the
            // segment tracking from here on: a body that has been pushed out of
            // a wall may have crossed a portal doing it.
            dvk::sim::CollisionConfig collision;
            collision.scrapeDamping = flight.wallScrape;
            cur.ship.body.radius = flight.shipRadius;
            // The shut-door faces for this step, built before anything asks a
            // geometric question.
            dvk::sim::buildSolidDoors(level, cur.game, solidDoors);


            dvk::sim::resolveCollision(level, cur.ship.body, collision,
                                       static_cast<float>(kSimStep), &contacts, &solidDoors);

            dvk::sim::FireControls fire;
            // Only the first sim step of a frame sees the button edges, for
            // the same reason it is the only one that sees the mouse motion.
            if (!consumedMouse) {
                fire.primary = input.mouseDown(GLFW_MOUSE_BUTTON_LEFT);
                fire.secondary = input.mouseDown(GLFW_MOUSE_BUTTON_RIGHT);
                fire.flare = input.pressed(dvk::keyFor(dvk::Toggle::Flare));
                for (int w = 0; w < dvk::sim::WeaponCount; ++w) {
                    if (input.pressed(dvk::kWeaponSelectBase + w)) fire.selectWeapon = w;
                }
                if (opts.autofire >= 0) {
                    if (opts.autofire >= dvk::sim::WeaponCount) {
                        fire.flare = true;
                    } else {
                        fire.selectWeapon = opts.autofire;
                        fire.primary = opts.autofire <= dvk::sim::WeaponVulcan;
                        fire.secondary = opts.autofire >= dvk::sim::WeaponConcussion;
                    }
                }
            }
            dvk::sim::stepCombat(level, cur.combat, cur.ship.body, cur.ship.orientation, weapons,
                                 fire, static_cast<float>(kSimStep), &cur.robots, &solidDoors,
                                 &soundEvents);
            dvk::sim::stepRobots(level, robotConfig, weapons, cur.robots, cur.combat,
                                 cur.ship.body, static_cast<float>(kSimStep), &solidDoors,
                                 &soundEvents);
            dvk::sim::stepGame(level, gameConfig, cur.game, cur.combat, cur.robots, robotConfig,
                               cur.ship.body, static_cast<float>(kSimStep), &soundEvents);
            // In M5 zero shields respawned you on the spot. From M7 it ends
            // the level, and the end screen offers a restart.
            consumedMouse = true;
            accumulator -= kSimStep;
            ++totalSteps;
            if (cur.game.phase == dvk::sim::GamePhase::Won ||
                cur.game.phase == dvk::sim::GamePhase::Lost) {
                session.finish();
                menu.showSummary(cur.game.phase == dvk::sim::GamePhase::Won, cur.game.endReason,
                                 cur.game.elapsed, cur.game.robotsDestroyed,
                                 cur.game.robotsAtStart);
                input.setMouseCaptured(window.handle(), false);
                accumulator = 0.0;
            }
        }
        // CLAUDE.md says the sim step does not allocate. M8 says to instrument
        // that rather than believe it. The first few frames are exempt: pools
        // and vectors reserve on their way to steady state, and counting those
        // would report a leak that is really a warm-up.
        const uint64_t simAllocCount = simAllocs.count();
        if (frame > kAllocWarmupFrames) {
            worstSimAllocs = std::max(worstSimAllocs, simAllocCount);
            simAllocFrames += simAllocCount > 0 ? 1 : 0;
            if (simAllocCount > 0 && firstSimAllocFrame < 0) firstSimAllocFrame = frame;
        }

        const auto alpha = static_cast<float>(accumulator / kSimStep);
        dvk::RenderState rs = dvk::interpolate(prev, cur, alpha);
        rs.camera = camera;

        // The mine coming apart. Driven from wall-clock rather than the sim, so
        // the shake is smooth at any frame rate and never accumulates into the
        // ship's actual position.
        // A missile leaving the tube shoves the view back, and only the view:
        // the ship's own motion is the simulation's business and a camera
        // effect must not reach into it. Squared so it snaps and then eases,
        // which reads as a shove rather than a wobble.
        if (cur.combat.player.recoilTimer > 0.0f) {
            const float k = cur.combat.player.recoilTimer / dvk::sim::kRecoilTime;
            const glm::vec3 back =
                glm::mat3_cast(rs.shipOrientation) * glm::vec3(0.0f, 0.0f, 1.0f);
            rs.shakeOffset += back * (0.55f * k * k);
        }

        if (cur.game.shake > 0.0f) {
            const auto t = static_cast<float>(clock.seconds());
            const float amp = cur.game.shake * 0.9f;
            rs.shakeOffset = glm::vec3(std::sin(t * 41.0f) + std::sin(t * 17.0f) * 0.6f,
                                       std::sin(t * 37.0f + 1.7f) + std::sin(t * 23.0f) * 0.5f,
                                       std::sin(t * 29.0f + 0.4f)) * amp * 0.5f;
            // A flicker that dips rather than brightens: the lights are failing.
            const float flicker = 0.72f + 0.28f * std::sin(t * 26.0f) * std::sin(t * 7.3f);
            rs.staticLightScale = 1.0f - cur.game.shake * (1.0f - flicker);
        }

        // --- sound -------------------------------------------------------
        //
        // The listener is the ship, not the camera. In chase view the camera
        // sits several units behind the hull, and hearing the mine from back
        // there would put the player's own guns slightly in front of them. The
        // chase camera is a view of the ship; it is not where the pilot is.
        //
        // It is set before the events are submitted so that this frame's sounds
        // are positioned against this frame's listener rather than the last
        // one's - at 200 units/s that is the difference between a bolt sounding
        // ahead of the ship and behind it.
        audio.setListener(rs.shipPos, rs.shipOrientation);
        audio.submit(soundEvents);

        // The drive. Gained and pitched by speed rather than by the throttle
        // key, so a ship coasting with the throttle released still sounds like
        // it is moving, which is what a player watching the walls go past
        // expects to hear.
        //
        // k is measured from kEngineSilentBelow rather than from zero, so the
        // drive fades in from nothing as the ship starts moving instead of
        // snapping on at some audible level. A gain of zero stops the channel;
        // because the ramp starts at zero, the restart when the ship moves off
        // again happens at silence and cannot be heard.
        {
            const float top = std::max(kEngineSilentBelow + 1.0f, flight.maxSpeed);
            const float k = std::clamp((rs.speed - kEngineSilentBelow) /
                                           (top - kEngineSilentBelow),
                                       0.0f, 1.0f);
            audio.setAmbient(dvk::audio::Ambient::Engine, kEngineGain * k,
                             kEngineLowPitch + (kEngineHighPitch - kEngineLowPitch) * k);
        }
        // The klaxon runs for exactly as long as the countdown does. Gained by
        // how far the mine has come apart, so it arrives under the first
        // explosion rather than on top of it.
        audio.setAmbient(dvk::audio::Ambient::Alarm,
                         cur.game.phase == dvk::sim::GamePhase::Countdown
                             ? 0.25f + 0.35f * cur.game.shake
                             : 0.0f);

        rs.pickupCount = static_cast<int>(
            std::min(level.pickups.size(), static_cast<size_t>(dvk::sim::kMaxPickups)));
        for (int i = 0; i < rs.pickupCount; ++i) {
            const dvk::Pickup& pickup = level.pickups[static_cast<size_t>(i)];
            dvk::RenderState::PickupView& view = rs.pickups[static_cast<size_t>(i)];
            view.present = !cur.game.collected[static_cast<size_t>(i)];
            view.position = pickup.position;
            const float pulse = 0.85f + 0.15f * std::sin(static_cast<float>(clock.seconds()) * 3.0f +
                                                         static_cast<float>(i));
            switch (pickup.kind) {
                case dvk::PickupKind::Shield: view.color = glm::vec3(0.4f, 1.0f, 0.55f); break;
                case dvk::PickupKind::Energy: view.color = glm::vec3(0.35f, 0.8f, 1.0f); break;
                case dvk::PickupKind::LaserUpgrade: view.color = glm::vec3(1.0f, 0.55f, 0.9f); break;
                case dvk::PickupKind::Concussion: view.color = glm::vec3(1.0f, 0.6f, 0.3f); break;
                case dvk::PickupKind::Homing: view.color = glm::vec3(0.6f, 0.85f, 1.0f); break;
                case dvk::PickupKind::Flares: view.color = glm::vec3(1.0f, 0.8f, 0.4f); break;
                case dvk::PickupKind::KeyBlue: view.color = glm::vec3(0.35f, 0.55f, 1.0f); break;
                case dvk::PickupKind::KeyYellow: view.color = glm::vec3(1.0f, 0.9f, 0.3f); break;
                case dvk::PickupKind::KeyRed: view.color = glm::vec3(1.0f, 0.35f, 0.3f); break;
                case dvk::PickupKind::Count: break;
            }
            view.color *= pulse;
            view.size = pickup.kind >= dvk::PickupKind::KeyBlue ? 1.1f : 0.85f;
        }

        // Every light that is not baked into the level, rebuilt each frame.
        // The vector keeps its capacity and each Light keeps its segment list's
        // capacity, so after the first few frames this allocates nothing.
        //
        // The headlight is always index 0: the renderer uses that to know which
        // light the ship must never shadow.
        const glm::mat3 shipBasis = glm::mat3_cast(rs.shipOrientation);
        const glm::vec3 shipForward = shipBasis * glm::vec3(0.0f, 0.0f, -1.0f);

        size_t dynamicUsed = 0;
        const auto nextLight = [&]() -> dvk::Light& {
            if (dynamicUsed == dynamicLights.size()) dynamicLights.emplace_back();
            return dynamicLights[dynamicUsed++];
        };

        {
            dvk::Light& hl = nextLight();
            hl.type = dvk::LightType::Spot;
            hl.radius = kHeadlightRadius;
            hl.coneDegrees = kHeadlightCone;
            hl.color = glm::vec3(1.0f, 0.97f, 0.9f);
            hl.intensity = cur.combat.player.headlightOn ? kHeadlightIntensity : 0.0f;
            hl.castsShadows = cur.combat.player.headlightOn;
            hl.position = rs.shipPos + shipForward * kHeadlightForward;
            hl.direction = shipForward;
            hl.segment = rs.shipSegment;
            hl.segments.clear();
            if (cur.combat.player.headlightOn) {
                dvk::rebuildLightSegments(level, hl, flood, &cur.game.doorField);
            }
        }

        if (cur.game.doorField.opaqueCount() != lightDoorSignature) {
            lightDoorSignature = cur.game.doorField.opaqueCount();
            dvk::rebuildAllLightSegments(level, flood, &cur.game.doorField);
        }

        // Every bolt in flight is a moving light. A laser going down a corridor
        // sweeping shadows off everything in it is the single most cinematic
        // thing the technique buys, and it costs one flood fill per bolt over a
        // radius small enough that the fill stops almost immediately.
        const auto addPointLight = [&](const glm::vec3& position, int segment,
                                       const glm::vec3& color, float radius, float intensity) {
            if (radius <= 0.0f || intensity <= 0.0f) return;
            dvk::Light& l = nextLight();
            l.type = dvk::LightType::Point;
            l.position = position;
            l.segment = segment;
            l.color = color;
            l.intensity = intensity;
            l.radius = radius;
            l.castsShadows = true;
            l.segments.clear();
            dvk::rebuildLightSegments(level, l, flood, &cur.game.doorField);
        };

        for (const dvk::sim::Projectile& p : cur.combat.projectiles.items) {
            if (!p.alive) continue;
            addPointLight(p.position, p.segment, p.color, p.lightRadius, p.lightIntensity);
        }
        for (const dvk::sim::Flare& f : cur.combat.flares.items) {
            if (!f.alive) continue;
            // Fades over the last few seconds rather than snapping out.
            const float remaining = f.lifetime - f.age;
            const float fade = std::clamp(remaining / weapons.flareFade, 0.0f, 1.0f);
            addPointLight(f.position, f.segment, weapons.flareColor, weapons.flareRadius,
                          weapons.flareIntensity * fade);
        }
        for (const dvk::sim::Effect& e : cur.combat.effects.items) {
            if (!e.alive || e.lightIntensity <= 0.0f) continue;
            const float k = e.lifetime > 0.0f ? e.age / e.lifetime : 1.0f;
            addPointLight(e.position, e.segment, e.color, e.lightRadius,
                          e.lightIntensity * std::max(0.0f, 1.0f - k));
        }

        dynamicLights.resize(dynamicUsed);

        // How much light is actually falling on the player, as the simulation
        // understands it rather than as the screen shows it. Asked once per
        // frame, after both light lists exist, over the ship's collision sphere
        // so the answer does not flicker as a shadow edge crosses a point.
        // Nothing consumes it yet beyond the debug readout; it is here because
        // "is the player in darkness" is a question about the world that the
        // world should be able to answer.
        dvk::sim::IlluminationQuery lightQuery;
        lightQuery.point = rs.shipPos;
        lightQuery.segment = rs.shipSegment;
        lightQuery.bodyRadius = cur.ship.body.radius;
        playerLight = dvk::sim::Illumination{};
        dvk::sim::addIllumination(playerLight, lightQuery, level, level.lights, &solidDoors);
        dvk::sim::addIllumination(playerLight, lightQuery, level, dynamicLights, &solidDoors);

        // A ray straight out of the nose, for the debug overlay and as a live
        // check that the query everything from M5 on depends on agrees with
        // what is on screen.
        const dvk::sim::RayHit noseRay =
            dvk::sim::raycast(level, rs.shipPos, shipForward, 300.0f, rs.shipSegment);

        const glm::ivec2 fb = window.framebufferSize();
        dvk::alloc::Scope drawAllocs;
        renderer.beginFrame(fb.x, fb.y);
        renderer.buildDebugLines(level, rs, contacts, noseRay);
        if (!freezeVisible) {
            walk.build(level, rs.shipSegment, renderer.viewProj(rs), rs.shipPos,
                       &cur.game.doorField);
        }

        dvk::gl::HudData hud;
        hud.show = !opts.noUi && session.active() &&
                   session.mode() != dvk::AppMode::RunSummary;
        hud.shields = cur.combat.player.shields;
        hud.energy = cur.combat.player.energy;
        hud.weaponName = dvk::sim::kWeaponNames[static_cast<size_t>(cur.combat.player.weapon)];
        hud.laserLevel = cur.combat.player.weapon == dvk::sim::WeaponLaser
                             ? cur.combat.player.laserLevel
                             : 0;
        switch (cur.combat.player.weapon) {
            case dvk::sim::WeaponVulcan: hud.ammo = cur.combat.player.vulcanAmmo; break;
            case dvk::sim::WeaponConcussion: hud.ammo = cur.combat.player.concussionAmmo; break;
            case dvk::sim::WeaponHoming: hud.ammo = cur.combat.player.homingAmmo; break;
            default: hud.ammo = -1; break;
        }
        hud.flares = cur.combat.player.flares;
        for (int k = 0; k < 3; ++k) hud.keys[k] = cur.game.keys[static_cast<size_t>(k)];
        hud.countdown = cur.game.phase == dvk::sim::GamePhase::Countdown ? cur.game.countdown
                                                                        : -1.0f;
        hud.phase = static_cast<int>(cur.game.phase);
        hud.message = cur.game.endReason;
        hud.elapsed = cur.game.elapsed;
        hud.robotsDestroyed = cur.game.robotsDestroyed;
        hud.damageFlash = cur.combat.player.hurtTimer / dvk::sim::kHurtFlashTime;
        hud.robotsTotal = cur.game.robotsAtStart;

        dvk::MenuView menuView = menu.view(fb.x, fb.y);
        if (opts.noUi) menuView.screen = dvk::MenuScreen::Hidden;

        if (automap.open) {
            automap.segmentCount = level.segmentCount();
            automap.visitedCount =
                opts.reveal ? level.segmentCount() : cur.game.visitedCount;
            for (int si = 0; si < automap.segmentCount &&
                             si < dvk::gl::AutomapData::kMaxSegments; ++si) {
                automap.visited[static_cast<size_t>(si)] =
                    opts.reveal || cur.game.visited[static_cast<size_t>(si)];
            }
            automap.shipPosition = rs.shipPos;
            automap.shipForward = shipForward;
            automap.shipSegment = rs.shipSegment;

            automap.markerCount = 0;
            const auto addMarker = [&](const glm::vec3& p, const glm::vec3& color, float size,
                                       bool hollow) {
                if (automap.markerCount >= dvk::gl::AutomapData::kMaxMarkers) return;
                dvk::gl::AutomapData::Marker& m =
                    automap.markers[static_cast<size_t>(automap.markerCount++)];
                m.position = p;
                m.color = color;
                m.size = size;
                m.hollow = hollow;
            };

            // Keys the player has laid eyes on, whether or not they picked them
            // up. Anything else on the map would be telling them where to go.
            for (int pi = 0; pi < rs.pickupCount; ++pi) {
                const dvk::Pickup& pickup = level.pickups[static_cast<size_t>(pi)];
                if (pickup.kind < dvk::PickupKind::KeyBlue) continue;
                if (!opts.reveal && !cur.game.seen[static_cast<size_t>(pi)]) continue;
                const bool collected = cur.game.collected[static_cast<size_t>(pi)];
                glm::vec3 color(1.0f);
                switch (pickup.kind) {
                    case dvk::PickupKind::KeyBlue: color = glm::vec3(0.35f, 0.55f, 1.0f); break;
                    case dvk::PickupKind::KeyYellow: color = glm::vec3(1.0f, 0.9f, 0.3f); break;
                    case dvk::PickupKind::KeyRed: color = glm::vec3(1.0f, 0.35f, 0.3f); break;
                    default: break;
                }
                addMarker(pickup.position, color, 1.8f, !collected);
            }

            if (cur.game.reactorSeen || opts.reveal) {
                // Green once it is gone, so a glance says whether the job is
                // done and the map is now an escape route.
                const glm::vec3 color = cur.game.reactorAlive ? glm::vec3(1.0f, 0.45f, 0.2f)
                                                              : glm::vec3(0.4f, 1.0f, 0.5f);
                glm::vec3 at = cur.game.reactorPosition;
                if (opts.reveal && !cur.game.reactorSeen) {
                    for (int ri = 0; ri < cur.robots.count; ++ri) {
                        const dvk::sim::Robot& r = cur.robots.robots[static_cast<size_t>(ri)];
                        if (robotConfig.types[static_cast<size_t>(r.type)].isReactor) {
                            at = r.body.position;
                        }
                    }
                }
                addMarker(at, color, 3.0f, false);
            }
            if (cur.game.exitOpen && level.exitTrigger.present) {
                addMarker(level.exitTrigger.position, glm::vec3(0.5f, 1.0f, 0.7f), 3.0f, false);
            }

            renderer.drawAutomap(automap, level, hud, menuView);
        } else {
            renderer.drawScene(rs, level, walk, dynamicLights, weapons.shadowBudget, hud,
                               menuView);
        }
        renderer.resolve();
        // The same question for the draw path: everything from beginFrame to
        // resolve, which is where per-frame geometry is built and submitted.
        const uint64_t drawAllocCount = drawAllocs.count();
        if (frame > kAllocWarmupFrames) {
            worstDrawAllocs = std::max(worstDrawAllocs, drawAllocCount);
            drawAllocFrames += drawAllocCount > 0 ? 1 : 0;
            if (drawAllocCount > 0 && firstDrawAllocFrame < 0) firstDrawAllocFrame = frame;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        // --no-ui still runs a frame, so ImGui's internal state stays
        // consistent; it just does not open any window.
        // ImGui is debug-only from M7: the HUD carries what the player needs.
        if (showDebugUi) {
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
            ImGui::Separator();
            const dvk::sim::PlayerState& pl = cur.combat.player;
            ImGui::Text("shields %5.1f   energy %5.1f", pl.shields, pl.energy);
            ImGui::Text("weapon: %s%s", dvk::sim::kWeaponNames[static_cast<size_t>(pl.weapon)],
                        pl.weapon == dvk::sim::WeaponLaser ? "" : "");
            if (pl.weapon == dvk::sim::WeaponLaser) {
                ImGui::SameLine();
                ImGui::Text("level %d", pl.laserLevel);
            }
            ImGui::Text("vulcan %d   concussion %d   homing %d   flares %d", pl.vulcanAmmo,
                        pl.concussionAmmo, pl.homingAmmo, pl.flares);
            ImGui::Text("%d bolts, %d flares burning, %d effects, %d deaths",
                        cur.combat.projectiles.count(), cur.combat.flares.count(),
                        cur.combat.effects.count(), pl.deaths);
            ImGui::Separator();
            ImGui::Text("contacts %d in %d iterations", contacts.count, contacts.iterations);
            if (noseRay.hit) {
                ImGui::Text("nose ray: %.2f units, segment %d, triangle %d", noseRay.distance,
                            noseRay.segment, noseRay.triangle);
            } else {
                ImGui::TextDisabled("nose ray: no hit");
            }
            ImGui::Checkbox("wall normals", &renderer.drawWallNormals);
            ImGui::SameLine();
            ImGui::Checkbox("contacts", &renderer.drawContacts);
            ImGui::SameLine();
            ImGui::Checkbox("nose ray", &renderer.drawNoseRay);
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(12, 210), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(390, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("gpu timings")) {
            const dvk::gl::GpuTimers& gt = renderer.timers();
            const dvk::gl::Renderer::Stats& st = renderer.stats();
            if (!gt.valid()) {
                ImGui::TextUnformatted("waiting for the first results...");
                ImGui::TextUnformatted("(read back two frames late, so nothing stalls)");
            } else {
                const float total = gt.pass(dvk::gl::GpuTimers::Prepass) +
                                    gt.pass(dvk::gl::GpuTimers::Lights) +
                                    gt.pass(dvk::gl::GpuTimers::Unlit);
                ImGui::Text("depth prepass %6.3f ms", gt.pass(dvk::gl::GpuTimers::Prepass));
                ImGui::Text("all lights    %6.3f ms   (%d drawn)",
                            gt.pass(dvk::gl::GpuTimers::Lights), gt.lightCount());
                ImGui::Text("  stencil     %6.3f ms   (first %d slots)", gt.stencilTotal(),
                            gt.timedLightCount());
                ImGui::Text("  lighting    %6.3f ms   (first %d slots)", gt.lightingTotal(),
                            gt.timedLightCount());
                ImGui::Text("unlit         %6.3f ms", gt.pass(dvk::gl::GpuTimers::Unlit));
                ImGui::Separator();
                ImGui::Text("gpu total     %6.3f ms", total);
                if (gt.worstLight() >= 0) {
                    ImGui::Text("most expensive light: %d at %.3f ms", gt.worstLight(),
                                gt.worstLightMs());
                }
                ImGui::Separator();
                ImGui::BeginChild("per light gpu", ImVec2(0, 150), true);
                for (int li = 0; li < gt.timedLightCount(); ++li) {
                    ImGui::Text("%2d  stencil %6.3f   light %6.3f", li, gt.stencil(li),
                                gt.lighting(li));
                }
                ImGui::EndChild();
            }
            ImGui::Separator();
            ImGui::Text("shadow casters %d   volume tris %d", st.shadowCasters,
                        st.volumeTriangles);
            // Reported, not enforced: truncating a volume would reopen the
            // open-volume bug the shadow-closure work fixed.
            ImGui::Text("heaviest volume: light %d, %d triangles", st.worstVolumeLight,
                        st.worstVolumeTriangles);
            ImGui::Separator();
            const int measured = std::max(0, frame - kAllocWarmupFrames);
            ImGui::Text("heap allocations after a %d-frame warm-up:", kAllocWarmupFrames);
            ImGui::Text("  sim step:  %llu on %d of %d frames",
                        static_cast<unsigned long long>(worstSimAllocs), simAllocFrames, measured);
            ImGui::Text("  draw path: %llu on %d of %d frames",
                        static_cast<unsigned long long>(worstDrawAllocs), drawAllocFrames,
                        measured);
        }
        ImGui::End();

        if (ImGui::Begin("flight tuning")) {
            ImGui::TextDisabled("linear");
            ImGui::SliderFloat("forward thrust", &flight.forwardThrust, 0.0f, 200.0f, "%.1f");
            ImGui::SliderFloat("strafe thrust", &flight.strafeThrust, 0.0f, 200.0f, "%.1f");
            ImGui::SliderFloat("lift thrust", &flight.liftThrust, 0.0f, 200.0f, "%.1f");
            ImGui::SliderFloat("linear damping", &flight.linearDamping, 0.1f, 12.0f, "%.2f /s");
            ImGui::SliderFloat("max speed", &flight.maxSpeed, 1.0f, 80.0f, "%.1f");
            ImGui::SliderFloat("ship radius", &flight.shipRadius, 0.2f, 6.0f, "%.2f");
            ImGui::SliderFloat("wall scrape", &flight.wallScrape, 0.0f, 12.0f, "%.2f /s");
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
            ImGui::Checkbox("headlight (H)", &cur.combat.player.headlightOn);

            ImGui::Text("%d casters, %d silhouette edges, %d volume triangles",
                        st.shadowCasters, st.silhouetteEdges, st.volumeTriangles);
            ImGui::Text("shadow budget: %d of %d used, %d lights lit without casting",
                        st.shadowLights, st.shadowBudget, st.budgetDenied);
            ImGui::SliderInt("budget", &weapons.shadowBudget, 1, 32);
            ImGui::Text("%d billboards", st.billboards);

            ImGui::Separator();
            // What the simulation believes about the light on the player, next
            // to what the renderer did, because the two answer the same
            // question by different means and the panel is where they can be
            // caught disagreeing. The query traces walls and doors only, so a
            // robot standing in the beam darkens the picture but not this
            // number.
            ImGui::Text("light on the player: %.3f  (%s)", playerLight.value,
                        playerLight.dark() ? "DARK" : "lit");
            ImGui::Text("  %.0f%% of the hull lit, brightest light %.3f, threshold %.2f",
                        playerLight.litFraction() * 100.0f, playerLight.brightest,
                        static_cast<double>(dvk::sim::kDarkThreshold));
            ImGui::Text("  %d lights reach, %d unoccluded, %d shadow rays",
                        playerLight.lightsReaching, playerLight.lightsVisible,
                        playerLight.raysCast);

            ImGui::Separator();
            ImGui::TextDisabled("per light: edges / volume triangles / segments");
            ImGui::BeginChild("per light", ImVec2(0, 150), true);
            const auto& ls = renderer.lightStats();
            for (size_t i = 0; i < ls.size(); ++i) {
                const bool isStatic = i < level.lights.size();
                const char* label = isStatic ? "light" : (i == level.lights.size() ? "headlight"
                                                                                  : "dynamic");
                ImGui::PushID(static_cast<int>(i));
                if (isStatic) {
                    ImGui::Checkbox("##casts", &level.lights[i].castsShadows);
                } else {
                    ImGui::TextDisabled("  ");
                }
                ImGui::SameLine();
                ImGui::Text("%-9s %4d edges  %5d tris  %2d segs", label, ls[i].silhouetteEdges,
                            ls[i].volumeTriangles, ls[i].segments);
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(860, 12), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(510, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("robots")) {
            ImGui::Text("%d of %d alive   %d debris   %d drops", cur.robots.aliveCount(),
                        cur.robots.count,
                        static_cast<int>(std::count_if(
                            cur.robots.debris.begin(), cur.robots.debris.end(),
                            [](const dvk::sim::Debris& d) { return d.alive; })),
                        static_cast<int>(std::count_if(
                            cur.robots.drops.begin(), cur.robots.drops.end(),
                            [](const dvk::sim::Drop& d) { return d.alive; })));
            ImGui::Text("path field built for player segment %d", cur.robots.pathSegment);
            ImGui::Separator();
            ImGui::BeginChild("robot list", ImVec2(0, 220), true);
            for (int i = 0; i < cur.robots.count; ++i) {
                const dvk::sim::Robot& r = cur.robots.robots[static_cast<size_t>(i)];
                if (!r.alive) {
                    ImGui::TextDisabled("%2d  destroyed", i);
                    continue;
                }
                const dvk::sim::RobotDef& def =
                    robotConfig.types[static_cast<size_t>(r.type)];
                const int16_t distance =
                    r.body.segment >= 0 && r.body.segment < cur.robots.pathValidCount
                        ? cur.robots.distanceToPlayer[static_cast<size_t>(r.body.segment)]
                        : -1;
                ImGui::Text("%2d %-7s %-7s <- %-7s  %4.0f/%-4.0f hp  seg %2d  %2d away  "
                            "aware %.2f",
                            i, def.name.c_str(),
                            dvk::sim::kRobotModeNames[static_cast<size_t>(r.mode)],
                            dvk::sim::kRobotModeNames[static_cast<size_t>(r.previousMode)],
                            r.health, def.health, r.body.segment, distance, r.awareness);
            }
            ImGui::EndChild();
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(860, 300), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(430, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("sound")) {
            const dvk::audio::Mixer& mixer = audio.mixer();
            ImGui::Text("device %s   %d clip(s)   %d/%d voices",
                        opts.noSound ? "disabled"
                                     : (audio.deviceOpen() ? "open" : "none (silent)"),
                        static_cast<int>(audio.bank().clips.size()), mixer.activeVoices(),
                        dvk::audio::kMaxVoices);
            ImGui::Text("%u started, %u stolen, %u event(s), %u dropped",
                        static_cast<unsigned>(mixer.started()),
                        static_cast<unsigned>(mixer.stolen()), audio.totalPlayed(),
                        audio.droppedEvents());

            float volume = audio.masterGain();
            if (ImGui::SliderFloat("volume", &volume, 0.0f, 1.0f, "%.2f")) {
                audio.setMasterGain(volume);
            }
            bool isMuted = audio.muted();
            if (ImGui::Checkbox("mute (M)", &isMuted)) audio.setMuted(isMuted);

            // Editable, because the only way to tell whether a mine sounds
            // right is to fly it while changing these. They are not saved:
            // settling on a number means editing the default in mixer.h.
            ImGui::SeparatorText("attenuation");
            ImGui::SliderFloat("reference", &audio.mixer().attenuation.reference, 1.0f, 60.0f,
                               "%.0f units");
            ImGui::SliderFloat("maximum", &audio.mixer().attenuation.maximum, 40.0f, 500.0f,
                               "%.0f units");
            ImGui::SliderFloat("rolloff", &audio.mixer().attenuation.rolloff, 0.1f, 4.0f, "%.2f");

            // Every cue, including the ones that have never fired: a cue
            // sitting at zero for a whole level is the interesting case, and a
            // list of only what has played would be the one list that cannot
            // show it.
            ImGui::SeparatorText("cues played");
            ImGui::BeginChild("cue list", ImVec2(0, 180), true);
            const auto& played = audio.played();
            for (size_t i = 0; i < played.size(); ++i) {
                if (played[i] == 0) {
                    ImGui::TextDisabled("%-18s      -", dvk::sim::kCueNames[i]);
                } else {
                    ImGui::Text("%-18s %6u", dvk::sim::kCueNames[i], played[i]);
                }
            }
            ImGui::EndChild();
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(420, 300), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(430, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("weapons")) {
            ImGui::TextDisabled("1-4 select, left mouse fires, right mouse launches, F throws a flare");
            for (int w = 0; w < dvk::sim::WeaponCount; ++w) {
                dvk::sim::WeaponDef& def = weapons.weapons[static_cast<size_t>(w)];
                ImGui::PushID(w);
                if (ImGui::CollapsingHeader(dvk::sim::kWeaponNames[static_cast<size_t>(w)])) {
                    ImGui::SliderFloat("damage", &def.damage, 0.0f, 80.0f, "%.1f");
                    ImGui::SliderFloat("speed", &def.speed, 5.0f, 400.0f, "%.0f u/s");
                    ImGui::SliderFloat("fire interval", &def.fireInterval, 0.02f, 2.0f, "%.3f s");
                    ImGui::SliderFloat("energy cost", &def.energyCost, 0.0f, 8.0f, "%.2f");
                    ImGui::SliderFloat("spread", &def.spread, 0.0f, 0.15f, "%.4f rad");
                    ImGui::ColorEdit3("colour", &def.color.r);
                    ImGui::SliderFloat("light radius", &def.lightRadius, 0.0f, 60.0f, "%.1f");
                    ImGui::SliderFloat("light intensity", &def.lightIntensity, 0.0f, 8.0f, "%.2f");
                    if (def.splashRadius > 0.0f || w >= dvk::sim::WeaponConcussion) {
                        ImGui::SliderFloat("splash radius", &def.splashRadius, 0.0f, 30.0f, "%.1f");
                        ImGui::SliderFloat("splash damage", &def.splashDamage, 0.0f, 60.0f, "%.1f");
                    }
                    if (w == dvk::sim::WeaponHoming) {
                        ImGui::SliderFloat("turn rate", &def.turnRate, 0.0f, 8.0f, "%.2f rad/s");
                        ImGui::SliderFloat("seek cone", &def.seekCone, 0.0f, 1.6f, "%.2f rad");
                        ImGui::SliderFloat("seek range", &def.seekRange, 0.0f, 300.0f, "%.0f");
                    }
                }
                ImGui::PopID();
            }
            ImGui::Separator();
            ImGui::SliderInt("laser level", &cur.combat.player.laserLevel, 1,
                             dvk::sim::kLaserLevels);
            ImGui::SliderFloat("flare radius", &weapons.flareRadius, 2.0f, 80.0f, "%.1f");
            ImGui::SliderFloat("flare lifetime", &weapons.flareLifetime, 2.0f, 90.0f, "%.0f s");
            ImGui::SliderFloat("headlight drain", &weapons.headlightDrain, 0.0f, 10.0f, "%.2f /s");
            if (ImGui::Button("Save weapons")) {
                dvk::sim::saveWeaponConfig(kWeaponConfigPath, weapons);
            }
            ImGui::SameLine();
            if (ImGui::Button("Refill")) {
                cur.combat.player.shields = 100.0f;
                cur.combat.player.energy = 100.0f;
                cur.combat.player.vulcanAmmo = 2000;
                cur.combat.player.concussionAmmo = 8;
                cur.combat.player.homingAmmo = 6;
                cur.combat.player.flares = 12;
            }
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
                dvk::rebuildLightSegments(level, added, flood, &cur.game.doorField);
                level.lights.push_back(added);
                lightEditorSelection = static_cast<int>(level.lights.size()) - 1;
            }
            ImGui::SameLine();
            if (ImGui::Button("Save level")) dvk::saveLevel(levelPath, level);
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
                    dvk::rebuildLightSegments(level, l, flood, &cur.game.doorField);
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
        }
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
        (loopEnd > 0.0 ? loopEnd : clock.seconds()) - loopStart - minimizedSeconds -
        suspendedSeconds;
    dvk::log::info("exited cleanly: {} frames, {} sim steps in {:.3f}s ({:.1f} steps/s, target {:.0f})",
                   frame, totalSteps, elapsed,
                   elapsed > 0.0 ? static_cast<double>(totalSteps) / elapsed : 0.0, kSimHz);
    if (opts.resizeTest) {
        dvk::log::info("resize-test: {} frames ({:.2f}s) skipped while minimized, excluded above",
                       minimizedFrames, minimizedSeconds);
    }

    // What was actually heard. Sound is the one subsystem a screenshot cannot
    // show, so a run reports it rather than leaving "the guns make a noise" as a
    // claim about the code. A device of "none" is the normal answer in a
    // container and is not a failure; the cue counts are produced either way,
    // because they come from the simulation and not from the device.
    {
        const auto& played = audio.played();
        // Every cue that fired, with its count, rather than a total and the
        // loudest one. A cue that never fires is the interesting case - it means
        // either the level never reached that situation or nothing emits it -
        // and a summary line cannot tell those apart. This can.
        std::string cues;
        int distinct = 0;
        for (size_t i = 0; i < played.size(); ++i) {
            if (played[i] == 0) continue;
            ++distinct;
            if (!cues.empty()) cues += ", ";
            cues += std::format("{} x{}", dvk::sim::kCueNames[i], played[i]);
        }
        dvk::log::info("sound: device \"{}\"{}, {} events over {} cue(s), {} dropped",
                       opts.noSound ? "disabled" : (audio.deviceOpen() ? "open" : "none"),
                       audio.muted() ? ", muted" : "", audio.totalPlayed(), distinct,
                       audio.droppedEvents());
        if (distinct > 0) dvk::log::info("  {}", cues);
    }

    // The last frame's GPU cost, so a headless run reports it the way every
    // other number in this project is reported rather than only existing behind
    // a panel nobody can open without a display.
    {
        const dvk::gl::GpuTimers& gt = renderer.timers();
        if (gt.valid()) {
            const float total = gt.pass(dvk::gl::GpuTimers::Prepass) +
                                gt.pass(dvk::gl::GpuTimers::Lights) +
                                gt.pass(dvk::gl::GpuTimers::Unlit);
            dvk::log::info(
                "gpu (last frame): prepass {:.3f} ms, all lights {:.3f}, unlit {:.3f}, total "
                "{:.3f} ms over {} light(s); first {} slots: stencil {:.3f}, lighting {:.3f}; "
                "worst timed light {} at {:.3f} ms",
                gt.pass(dvk::gl::GpuTimers::Prepass), gt.pass(dvk::gl::GpuTimers::Lights),
                gt.pass(dvk::gl::GpuTimers::Unlit), total, gt.lightCount(),
                gt.timedLightCount(), gt.stencilTotal(), gt.lightingTotal(),
                gt.worstLight(), gt.worstLightMs());
        } else {
            dvk::log::info("gpu: no timestamp results resolved (too few frames, or unsupported)");
        }
    }

    // Measured, not asserted. A non-zero number here is a real finding: it means
    // something in the sim step or the draw path reached the heap on a frame
    // after warm-up, which CLAUDE.md says does not happen.
    const int measured = std::max(0, frame - kAllocWarmupFrames);
    if (measured > 0) {
        dvk::log::info(
            "allocations after a {}-frame warm-up: sim step {} on {} of {} frames (worst {}, "
            "first at frame {}), draw path {} on {} of {} (worst {}, first at frame {})",
            kAllocWarmupFrames, simAllocFrames == 0 ? "none" : "SOME", simAllocFrames, measured,
            worstSimAllocs, firstSimAllocFrame, drawAllocFrames == 0 ? "none" : "SOME",
            drawAllocFrames, measured, worstDrawAllocs, firstDrawAllocFrame);
    }
    return 0;
}
