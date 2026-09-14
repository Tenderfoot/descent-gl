#include "core/input.h"
#include "core/log.h"
#include "core/options.h"
#include "core/time.h"
#include "core/window.h"
#include "game/bindings.h"
#include "game/level.h"
#include "game/state.h"
#include "game/visibility.h"
#include "game/collision_test.h"
#include "game/loop_test.h"
#include "game/robots_test.h"
#include "game/weapons_test.h"
#include "game/world_test.h"
#include "render/debug.h"
#include "render/gl.h"
#include "render/renderer.h"
#include "render/screenshot.h"
#include "sim/collision.h"
#include "sim/combat.h"
#include "sim/flight.h"
#include "sim/flight_test.h"
#include "sim/gameflow.h"
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
    const char* levelPath = opts.level.empty() ? kDefaultLevelPath : opts.level.c_str();

    // Measures the flight model and exits. Deliberately before the window
    // exists: it needs no GL and no display, so it runs anywhere.
    if (opts.flightTest) {
        return dvk::sim::runFlightTest(kFlightConfigPath);
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

    dvk::SimState prev;
    dvk::SimState cur;
    cur.ship.body.position = level.start.position;
    cur.ship.body.segment = level.start.segment;
    cur.ship.orientation = level.start.orientation;
    if (opts.startSegment >= 0 && opts.startSegment < level.segmentCount()) {
        cur.ship.body.segment = opts.startSegment;
        cur.ship.body.position = level.segments[static_cast<size_t>(opts.startSegment)].center;
    }
    prev = cur;

    dvk::sim::ContactLog contacts;

    dvk::CameraMode camera =
        opts.chase ? dvk::CameraMode::Chase : dvk::CameraMode::FirstPerson;

    // F3 brings the debug panels back. Off by default, because from M7 the HUD
    // is what the player reads and the panels are for tuning.
    bool showDebugUi = !opts.noUi && opts.debugUi;

    dvk::PortalWalk walk;
    dvk::LightFlood flood;

    dvk::sim::WeaponConfig weapons = dvk::sim::loadWeaponConfig(kWeaponConfigPath);
    dvk::sim::initObjects(level, robotConfig, cur.combat.objects);
    dvk::sim::spawnRobots(level, robotConfig, cur.robots);

    const dvk::sim::GameConfig gameConfig;
    dvk::sim::initGame(level, cur.robots, cur.game);

    // Rebuilt each step and handed to collision, line of sight and projectiles
    // alike, so none of them can disagree about whether a door is shut.
    dvk::sim::SolidDoors solidDoors;

    // The level's own lights had their segment sets computed at load with no
    // doors. They are recomputed against the door state now, and again every
    // time a door changes between see-through and shut, so a light beside a
    // shut door stops at it the way a bolt light does.
    int lightDoorSignature = -1;

    // A fresh start, kept so any key can restart from an end screen without
    // reloading the level.
    const dvk::SimState initialState = cur;
    prev = cur;

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
        // Any key restarts from an end screen. Checked before the toggles
        // below so the restart key does not also flip the headlight.
        if (cur.game.phase == dvk::sim::GamePhase::Won ||
            cur.game.phase == dvk::sim::GamePhase::Lost) {
            if (input.anyKeyPressed() || input.mousePressed(GLFW_MOUSE_BUTTON_LEFT)) {
                cur = initialState;
                prev = cur;
                accumulator = 0.0;
            }
        } else if (input.pressed(GLFW_KEY_F3)) {
            showDebugUi = !showDebugUi;
        } else if (input.pressed(GLFW_KEY_H)) {
            // Refuses to come on with no energy left, which is the whole point
            // of it costing any.
            cur.combat.player.headlightOn =
                !cur.combat.player.headlightOn && cur.combat.player.energy > 0.0f;
        }
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
                fire.flare = input.pressed(GLFW_KEY_F);
                for (int w = 0; w < dvk::sim::WeaponCount; ++w) {
                    if (input.pressed(GLFW_KEY_1 + w)) fire.selectWeapon = w;
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
                                 fire, static_cast<float>(kSimStep), &cur.robots, &solidDoors);
            dvk::sim::stepRobots(level, robotConfig, weapons, cur.robots, cur.combat,
                                 cur.ship.body, static_cast<float>(kSimStep), &solidDoors);
            dvk::sim::stepGame(level, gameConfig, cur.game, cur.combat, cur.robots, robotConfig,
                               cur.ship.body, static_cast<float>(kSimStep));
            // In M5 zero shields respawned you on the spot. From M7 it ends
            // the level, and the end screen offers a restart.
            consumedMouse = true;
            accumulator -= kSimStep;
            ++totalSteps;
        }

        const auto alpha = static_cast<float>(accumulator / kSimStep);
        dvk::RenderState rs = dvk::interpolate(prev, cur, alpha);
        rs.camera = camera;

        // The mine coming apart. Driven from wall-clock rather than the sim, so
        // the shake is smooth at any frame rate and never accumulates into the
        // ship's actual position.
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
            hl.intensity = cur.combat.player.headlightOn ? 3.2f : 0.0f;
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

        // A ray straight out of the nose, for the debug overlay and as a live
        // check that the query everything from M5 on depends on agrees with
        // what is on screen.
        const dvk::sim::RayHit noseRay =
            dvk::sim::raycast(level, rs.shipPos, shipForward, 300.0f, rs.shipSegment);

        const glm::ivec2 fb = window.framebufferSize();
        renderer.beginFrame(fb.x, fb.y);
        renderer.buildDebugLines(level, rs, contacts, noseRay);
        if (!freezeVisible) {
            walk.build(level, rs.shipSegment, renderer.viewProj(rs), rs.shipPos,
                       &cur.game.doorField);
        }

        dvk::gl::HudData hud;
        hud.show = !opts.noUi;
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
        hud.robotsTotal = cur.game.robotsAtStart;

        renderer.drawScene(rs, level, walk, dynamicLights, weapons.shadowBudget, hud);
        renderer.resolve();

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
