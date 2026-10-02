#include "audio/audio.h"
#include "audio/sound_test.h"
#include "core/input.h"
#include "core/log.h"
#include "core/options.h"
#include "core/time.h"
#include "core/window.h"
#include "game/asset_check.h"
#include "game/automap_build.h"
#include "game/bindings.h"
#include "game/comm_director.h"
#include "game/frame_report.h"
#include "game/hud_build.h"
#include "game/level.h"
#include "game/level_list.h"
#include "game/level_load.h"
#include "game/level_switch_test.h"
#include "game/state.h"
#include "game/tutorial.h"
#include "game/visibility.h"
#include "game/collision_test.h"
#include "game/light_test.h"
#include "game/loop_test.h"
#include "game/progress.h"
#include "game/proxies.h"
#include "game/resize_test.h"
#include "game/robots_test.h"
#include "game/comm_player.h"
#include "game/session.h"
#include "game/shell.h"
#include "game/sim_sets.h"
#include "game/script.h"
#include "game/settings.h"
#include "game/soundtrack.h"
#include "game/weapons_test.h"
#include "game/world_test.h"
#include "render/debug.h"
#include "render/debug_ui.h"
#include "render/comm_clip.h"
#include "render/gl.h"
#include "core/alloc_count.h"
#include "render/renderer.h"
#include "render/frame_prepare.h"
#include "render/screenshot.h"
#include "render/sprites.h"
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
#include <cmath>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
constexpr const char* kLevelDir = "assets/levels";
constexpr const char* kCampaignPath = "assets/config/campaign.json";
constexpr const char* kWeaponConfigPath = "assets/config/weapons.json";
constexpr const char* kRobotConfigPath = "assets/config/robots.json";
constexpr const char* kSoundDir = "assets/sounds";
constexpr const char* kCommRoot = "assets/comm";
constexpr const char* kMusicDir = "assets/music";

// Every headless check, in one place. They need no window and no GL, so they
// run anywhere the build does and they run before one is created. Returns the
// process exit code, or -1 when the command line asked for the game itself.
int runHeadlessTest(const dvk::Options& opts, const char* levelPath) {
    if (opts.settingsTest) return dvk::runSettingsTest();
    if (opts.progressTest) return dvk::runProgressTest();
    if (opts.musicTest) return dvk::runMusicTest();
    if (opts.commTest) {
        const int clip = dvk::gl::runCommTest(kCommRoot);
        const int player = dvk::runCommPlayerTest();
        return clip != 0 || player != 0 ? 1 : 0;
    }
    if (opts.scriptTest) return dvk::runScriptTest(opts.level);
    if (opts.flightTest) return dvk::sim::runFlightTest(kFlightConfigPath);
    if (opts.validateAssets) return dvk::validateAssets() == 0 ? 0 : 1;
    if (opts.worldTest) return dvk::runWorldTest(levelPath);
    if (opts.collisionTest) return dvk::runCollisionTest(levelPath, kFlightConfigPath);
    if (opts.weaponsTest) {
        return dvk::runWeaponsTest(levelPath, kWeaponConfigPath, kRobotConfigPath);
    }
    if (opts.robotsTest) {
        return dvk::runRobotsTest(levelPath, kWeaponConfigPath, kRobotConfigPath);
    }
    if (opts.loopTest) return dvk::runLoopTest(levelPath, kWeaponConfigPath, kRobotConfigPath);
    if (opts.lightTest) return dvk::runLightTest(levelPath);
    // The whole audio path with no device and no sound card. Sound is the one
    // subsystem a screenshot cannot show, so this is how a claim about it is
    // made checkable.
    if (opts.soundTest) return dvk::audio::runSoundTest(kSoundDir, kMusicDir);
    return -1;
}

// Which screen the run opens on. A command-line run enters the level directly;
// everything else opens the shell somewhere.
void openingScreen(const dvk::Options& opts, bool commandLineRun, dvk::Shell& shell,
                   dvk::LevelSession& session, dvk::CommDirector& comm) {
    if (commandLineRun) {
        session.start();
        comm.enterLevel();
        shell.menu().hide();
    } else if (opts.settingsMenu) {
        shell.menu().showMain();
        shell.menu().showSettings(shell.settings());
    } else if (opts.controlsMenu) {
        shell.menu().showMain();
        shell.menu().showSettings(shell.settings());
        shell.menu().showControls();
    } else if (opts.levelSelect || opts.levelSwitchTest) {
        shell.menu().showLevelSelect();
    } else {
        shell.menu().showMain();
    }
}

} // namespace

int main(int argc, char** argv) {
    const dvk::Options opts = dvk::Options::parse(argc, argv);
    const char* levelPath = opts.level.empty() ? kDefaultLevelPath : opts.level.c_str();

    if (const int code = runHeadlessTest(opts, levelPath); code >= 0) return code;

    // Every shipped asset, every run, before a window exists. A broken asset
    // then fails before anything is on screen rather than at the moment the
    // level that places it is first loaded.
    dvk::validateAssets();

    // The explicit std::filesystem::path around the override is not decoration:
    // libstdc++ refuses a conditional whose arms are path and std::string, so
    // without it the Linux build - the one an agent verifies milestones on -
    // does not compile.
    const std::filesystem::path settingsPath =
        opts.settingsPath.empty() ? dvk::defaultPlayerSettingsPath()
                                  : std::filesystem::path(opts.settingsPath);
    dvk::PlayerSettings settings = dvk::loadPlayerSettings(settingsPath);
    const std::filesystem::path progressPath =
        opts.progressPath.empty() ? dvk::defaultPlayerProgressPath()
                                  : std::filesystem::path(opts.progressPath);
    dvk::SaveSlots saves = dvk::loadSaveSlots(progressPath);
    if (opts.volumeOverride) settings.masterVolume = opts.volume;
    if (opts.sizeOverride) {
        settings.windowWidth = opts.width;
        settings.windowHeight = opts.height;
        settings.fullscreen = false;
    }

    // A frame-limited run must not be pinned to the refresh rate, except under
    // --resize-test, where the window manager needs real time to deliver
    // iconify and restore.
    const bool vsyncAllowed = opts.frames < 0 || opts.resizeTest;

    dvk::Window::Desc desc;
    desc.width = settings.windowWidth;
    desc.height = settings.windowHeight;
#ifdef DVK_DEBUG
    desc.debugContext = true;
#endif
    desc.vsync = settings.vsync && vsyncAllowed;
    desc.fullscreen = settings.fullscreen;

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

    // Not const and not the command line's string: the menu can load another
    // mine, and the light editor writes back to whichever one is loaded.
    std::string levelFile = levelPath;
    dvk::Level level = dvk::loadLevel(levelFile);
    const dvk::sim::RobotConfig robotConfig = [] {
        dvk::sim::RobotConfig cfg = dvk::sim::loadRobotConfig(kRobotConfigPath);
        dvk::resolveRobotAttachments(cfg);
        return cfg;
    }();
    dvk::gl::Renderer renderer(level, robotConfig);

    dvk::Clock clock;
    // Sound. A container with no sound card is the ordinary case for this and
    // is a warning, not an error: the mixer still runs, nothing pulls from it,
    // and every other number the run reports is unaffected.
    dvk::audio::Audio audio;

    dvk::CommDirector comm(renderer, audio, opts.comm);
    if (!opts.comm.empty() && !comm.knows(opts.comm)) {
        dvk::log::fatal("--comm names unknown clip '{}'", opts.comm);
    }
    // The transmission for the ship being disabled and starting to repair. Not
    // level content - a ship can be disabled in any mine - so it is raised here
    // rather than by a script. No words of its own: the panel reads "incoming
    // transmission" and runs for the clip's own length. Until the clip exists
    // the recovery simply plays nothing.
    const dvk::ScriptMessage recoveryMessage{"emergencyrecovery", "emergencyrecovery", "", "",
                                             0.0f};
    const bool recoveryClip = comm.knows(recoveryMessage.clip);

    dvk::TutorialPane tutorial(renderer, audio);
    dvk::ScriptHost script;
    dvk::LevelSession session;
    session.load(level, robotConfig, opts.startSegment);
    dvk::SimState& prev = session.previous();
    dvk::SimState& cur = session.current();

    dvk::sim::ContactLog contacts;
    dvk::gl::AutomapData automap;
    automap.open = opts.automap;
    // Where the wheel has asked the map to be. `automap.zoom` chases it rather
    // than snapping, so a click of the wheel reads as a move in and not as a
    // cut.
    float automapZoomTarget = automap.zoom;
    // The level's own lights had their segment sets computed at load with no
    // doors. They are recomputed against the door state now, and again every
    // time a door changes between see-through and shut, so a light beside a
    // shut door stops at it the way a bolt light does.
    int lightDoorSignature = -1;
    int lightEditorSelection = -1;

    dvk::LoadedLevel loaded{level,
                            levelFile,
                            renderer,
                            session,
                            script,
                            comm,
                            tutorial,
                            contacts,
                            automap,
                            automapZoomTarget,
                            lightDoorSignature,
                            lightEditorSelection,
                            robotConfig,
                            opts.startSegment,
                            opts.headlight};

    dvk::loadLevelScript(script, level, comm);
    dvk::applySessionLoadout(session, script.loadout());
    // What a run that never passes through the slot screen is played at. One
    // that does is set again from its slot when it starts.
    dvk::applySessionDifficulty(session, level, dvk::Difficulty::Normal);
    if (opts.headlight) dvk::setSessionHeadlight(session, true);
    if (opts.targetHud && session.initial().robots.count > 0) {
        dvk::sim::Robot& target = session.initial().robots.robots[0];
        const dvk::sim::RobotDef& def =
            robotConfig.types[static_cast<size_t>(target.type)];
        target.health = def.health * 0.09f;
        target.mode = dvk::sim::RobotMode::Disabled;
        session.initial().combat.player.lastTargetRobot = 0;
        session.initial().combat.player.targetHudTime = dvk::sim::kTargetHudTime;
    }

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

    // The level list's records are a save slot's, and are filled in when one
    // is opened.
    dvk::LevelCatalog catalog = dvk::discoverCatalog(kLevelDir, kCampaignPath);

    dvk::Shell shell(window, input, audio, session, loaded, std::move(settings), settingsPath,
                     std::move(saves), progressPath, vsyncAllowed);
    shell.menu().setLevels(std::move(catalog.levels), catalog.campaignCount);
    shell.refreshSlotSummaries();
    // The list opens on the level that is loaded, so confirming twice from the
    // main menu starts what a plain run has always started.
    for (int i = 0; i < static_cast<int>(shell.menu().levels().size()); ++i) {
        if (shell.menu().levels()[static_cast<size_t>(i)].path == levelFile) {
            shell.menu().setSelectedLevel(i);
        }
    }

    const bool commandLineRun =
        argc > 1 && !opts.startMenu && !opts.levelSelect && !opts.levelSwitchTest &&
        !opts.settingsMenu && !opts.controlsMenu;
    // A run somebody is actually playing, as opposed to one the command line is
    // driving. Two things hang off it and both need the same answer: a
    // screenshot pass must not write a run into the player's records, and it
    // must not be stopped dead by a tutorial pane either - it has nobody to
    // click the button. `commandLineRun` alone will not do, because it
    // deliberately excludes the checks that drive the menus themselves.
    const bool playerSession = !commandLineRun && !opts.levelSwitchTest && opts.frames < 0 &&
                               opts.screenshot.empty() && !opts.resizeTest;
    openingScreen(opts, commandLineRun, shell, session, comm);

    // Rebuilt every step and never resized, so the sim step still allocates
    // nothing. See kMaxCollisionBodies.
    dvk::sim::BodySet bodies;
    dvk::sim::BodyResolveStats bodyStats;
    // What stands between a light and a surface, or an eye and its target, and
    // is not the mine. Rebuilt every step, never resized.
    dvk::sim::OccluderSet occluders;

    if (!opts.noSound) {
        // assets/comm too: a transmission's voice track lives beside its
        // pictures rather than in assets/sounds, because which transmissions
        // exist is level content.
        audio.init(kSoundDir, /*openDevice=*/true, kCommRoot, kMusicDir);
    }
    shell.applyAudioSettings();
    // Filled by the simulation steps of one frame and drained once, after them.
    // Never cleared inside the step loop: a frame that runs three steps must
    // still hear all three.
    dvk::sim::EventQueue soundEvents;

    // Chase is the view the game opens in. --chase asked for exactly this
    // back when first person was the default, so it is now a no-op rather
    // than a removal: a command line or a habit that still passes it keeps
    // working and simply gets what it would have got anyway. F1 is how you
    // get to first person, and still is.
    dvk::CameraMode camera = dvk::CameraMode::Chase;

    // F3 brings the debug panels back. Off by default, because from M7 the HUD
    // is what the player reads and the panels are for tuning.
    bool showDebugUi = !opts.noUi && opts.debugUi;

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
    dvk::sim::PlayerVisibility playerSeen;

    // Rebuilt every frame and reused, so the vectors inside each Light keep
    // their capacity and the draw path does not allocate. Index 0 is always
    // the headlight, which is the convention the renderer relies on to know
    // which light the ship must not shadow.
    std::vector<dvk::Light> dynamicLights;
    dynamicLights.reserve(1 + dvk::sim::kMaxProjectiles + dvk::sim::kMaxFlares +
                          dvk::sim::kMaxEffects);

    // Freezing the visible set leaves the last portal walk in place while the
    // camera keeps moving, so the culling can be inspected from outside it.
    // Without this, the one thing you cannot see is what the cull decided.
    bool freezeVisible = false;

    dvk::ResizeTest resizeTest;
    dvk::LevelSwitchTest switchTest;
    switchTest.fromLevel = levelFile;
    switchTest.fromSegments = level.segmentCount();

    // Measured from here, not from construction: startup (window, GL, texture,
    // shader compile) runs no sim steps and would drag the average down.
    const double loopStart = clock.seconds();
    double lastTime = loopStart;
    double accumulator = 0.0;
    int pendingWeaponSelection = -1;
    bool pendingSecondaryCycle = false;
    bool pendingFlare = false;
    long long totalSteps = 0;
    int frame = 0;
    double frameMs = 0.0;
    // How far down the firing line the reticle is drawn, and what it is on.
    // Held across frames because it is chased rather than set: see where it is
    // updated below.
    dvk::AimLine aim;
    // Where the chase camera's eye ended up once the mine had its say, and
    // whether the ship is still far enough from it to draw. Held across frames
    // for the same reason: see where they are updated below.
    dvk::ChaseCamera chaseCamera;
    // Whether the soundtrack is in combat. Held across frames because the
    // fight being over is a thing that has to last.
    dvk::MusicDirector musicDirector;
    // The HUD marquee's own clock. Real time rather than simulation time: the
    // transports keep playing through a pause, so the scroll keeps moving with
    // them. Reset whenever the soundtrack stops, so the next level's readout
    // starts at the left rather than wherever the last one had got to.
    float musicMarqueeTime = 0.0f;
    // Storage the finished HUD points into, refilled in place every frame.
    dvk::HudScratch hudScratch;
    dvk::AllocationTally allocations;
    double loopEnd = 0.0;   // set on the final frame, before any screenshot write
    int minimizedFrames = 0;
    double minimizedSeconds = 0.0;
    double suspendedSeconds = 0.0;

    if (session.advancesSimulation() && opts.frames < 0 && !opts.resizeTest) {
        shell.captureMouse(true);
    }

    while (!window.shouldClose()) {
        const bool simulationWasAdvancing = session.advancesSimulation();
        input.beginFrame();
        window.poll();

        if (opts.resizeTest) {
            const double t = clock.seconds() - loopStart;
            resizeTest.update(window, t);
            if (dvk::ResizeTest::finished(t)) window.requestClose();
        }

        const double now = clock.seconds();
        double elapsed = now - lastTime;
        lastTime = now;
        frameMs = elapsed * 1000.0;
        elapsed = std::min(elapsed, kMaxFrameTime);
        accumulator += elapsed;

        shell.tickDisplayConfirmation(now);

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

        const dvk::InputBindings& bindings = shell.settings().bindings;
        const bool pausePressed = dvk::bindingPressed(input, bindings, dvk::PlayerAction::Pause);
        const bool menuEscape = input.pressed(GLFW_KEY_ESCAPE);
        const bool bindingCaptureActive = shell.capturingBinding();
        {
            const dvk::ShellEffect effect =
                shell.handlePauseKeys(pausePressed, menuEscape, now);
            if (effect.resetAccumulator) accumulator = 0.0;
            if (effect.resyncPrevious) prev = cur;
        }

        const glm::ivec2 menuFramebuffer = window.framebufferSize();
        const glm::ivec2 menuWindow = window.windowSize();
        const glm::vec2 pointerScale(
            static_cast<float>(menuFramebuffer.x) / static_cast<float>(std::max(menuWindow.x, 1)),
            static_cast<float>(menuFramebuffer.y) / static_cast<float>(std::max(menuWindow.y, 1)));
        dvk::MenuInput menuInput = dvk::readMenuInput(input, input.cursorPosition() * pointerScale);
        // The wheel scrolls the two lists that are longer than the screen, and
        // is taken only while one of them is up: the automap and the debug
        // panels want it the rest of the time.
        if (shell.menu().screen() == dvk::MenuScreen::LevelSelect ||
            shell.menu().screen() == dvk::MenuScreen::Controls) {
            menuInput.scroll = input.consumeScroll();
        }
        if (opts.levelSwitchTest) menuInput = dvk::LevelSwitchTest::menuInput(frame);
        const dvk::MenuAction menuAction =
            shell.menu().update(menuInput, menuFramebuffer.x, menuFramebuffer.y);
        {
            const dvk::ShellEffect effect = shell.applyMenuAction(menuAction, now);
            if (effect.resetAccumulator) accumulator = 0.0;
            if (effect.resyncPrevious) prev = cur;
        }

        if (!bindingCaptureActive && input.pressed(GLFW_KEY_F3)) {
            showDebugUi = !showDebugUi;
        } else if (session.mode() == dvk::AppMode::Playing &&
                   dvk::bindingPressed(input, bindings,
                                       dvk::PlayerAction::Headlight)) {
            // Refuses to come on with no energy left, which is the whole point
            // of it costing any.
            cur.combat.player.headlightOn =
                !cur.combat.player.headlightOn && cur.combat.player.energy > 0.0f;
        }
        if (!bindingCaptureActive && dvk::bindingPressed(input, bindings,
                                                         dvk::PlayerAction::ChaseCamera)) {
            camera = camera == dvk::CameraMode::FirstPerson ? dvk::CameraMode::Chase
                                                            : dvk::CameraMode::FirstPerson;
        }
        if (!bindingCaptureActive && session.mode() == dvk::AppMode::Playing &&
            dvk::bindingPressed(input, bindings, dvk::PlayerAction::Automap)) {
            automap.open = !automap.open;
        }
        if (!bindingCaptureActive &&
            dvk::bindingPressed(input, bindings, dvk::PlayerAction::NextTrack)) {
            // Which playlist to move is whichever one the player is hearing,
            // and mid-crossfade both of them are: there is no one answer, so
            // the key does nothing until the fade has landed. The same test
            // the HUD reads, so the hint is absent exactly when the key is.
            const float mix = audio.musicMix();
            if (audio.musicPlaying() && (mix <= 0.0f || mix >= 1.0f)) {
                audio.nextMusicTrack(mix >= 1.0f ? 1 : 0);
            }
        }
        if (!bindingCaptureActive &&
            dvk::bindingPressed(input, bindings, dvk::PlayerAction::Mute)) {
            shell.toggleMute();
        }

        if (!session.advancesSimulation()) {
            accumulator = 0.0;
            pendingWeaponSelection = -1;
            pendingSecondaryCycle = false;
            pendingFlare = false;
        } else if (!bindingCaptureActive) {
            // A fast render frame can have no fixed simulation step. Preserve
            // one-shot gameplay input until a step actually consumes it.
            pendingFlare = pendingFlare || dvk::bindingPressed(input, bindings,
                                                               dvk::PlayerAction::Flare);
            // A key each for the four primaries; the missiles share one that
            // cycles them, which the simulation resolves against whichever one
            // is selected at the time.
            for (const dvk::WeaponSelectAction& select : dvk::kWeaponSelectActions) {
                if (dvk::bindingPressed(input, bindings, select.action)) {
                    pendingWeaponSelection = select.weapon;
                }
            }
            pendingSecondaryCycle =
                pendingSecondaryCycle ||
                dvk::bindingPressed(input, bindings,
                                    dvk::PlayerAction::CycleSecondary);
        }

        dvk::alloc::Scope simAllocs;
        glm::vec2 mouseDelta{0.0f};
        // Drained every frame whether or not the map is open, and used only
        // when it is: ticks collected while flying would otherwise arrive as
        // one lurch the moment Tab is pressed. The debug panels get first
        // refusal, so the wheel over an open panel scrolls the panel.
        const float wheel = input.consumeScroll();
        // With the map open the mouse orbits it instead of steering. The
        // simulation is untouched either way: the ship keeps flying, robots keep
        // hunting, and the countdown keeps running - opening the map is not a
        // pause, and reading it while something is shooting at you is the cost
        // of looking.
        if (automap.open) {
            // The automap camera is render-driven, so it consumes motion every
            // frame. Ship steering consumes below only when a simulation step
            // actually runs, leaving motion queued across faster render frames.
            mouseDelta = input.consumeMouseDelta();
            automap.yaw += mouseDelta.x * 0.005f;
            automap.pitch = std::clamp(automap.pitch - mouseDelta.y * 0.005f, -1.45f, 1.45f);

            if (wheel != 0.0f && !ImGui::GetIO().WantCaptureMouse) {
                // Multiplied rather than added, because the camera's distance
                // is divided by the zoom: a constant factor per click is what
                // makes every click look like the same step, near or far.
                automapZoomTarget = std::clamp(
                    automapZoomTarget * std::pow(1.15f, wheel),
                    dvk::gl::AutomapData::kMinZoom, dvk::gl::AutomapData::kMaxZoom);
            }
            // Chased, not set, on the frame clock rather than the step clock,
            // because this is camera state and the map is drawn every frame.
            const float follow = 1.0f - std::exp(-12.0f * static_cast<float>(elapsed));
            automap.zoom += (automapZoomTarget - automap.zoom) * follow;
        }
        bool consumedMouse = false;
        bool runFinished = false;
        bool shipDisabled = false;
        soundEvents.clear();
        while (session.advancesSimulation() && accumulator >= kSimStep) {
            prev = cur;
            // The whole frame's motion belongs to the first step; later steps
            // in the same frame have no new input to apply.
            if (!consumedMouse && !automap.open) {
                mouseDelta = input.consumeMouseDelta() * shell.settings().mouseSensitivity;
                if (shell.settings().invertMouseY) mouseDelta.y = -mouseDelta.y;
            }
            dvk::sim::ShipControls controls =
                dvk::readControls(input, consumedMouse ? glm::vec2(0.0f) : mouseDelta,
                                  bindings);
            // Recomputed every step rather than latched, so a shield pickup
            // hands the ship straight back.
            cur.ship.disabled = dvk::sim::playerDisabled(cur.combat.player);
            dvk::sim::stepShip(cur.ship, controls, flight, static_cast<float>(kSimStep));

            // Collision runs after integration, every step, and owns the
            // segment tracking from here on: a body that has been pushed out of
            // a wall may have crossed a portal doing it.
            dvk::sim::CollisionConfig collision;
            collision.scrapeDamping = flight.wallScrape;
            cur.ship.body.radius = flight.shipRadius;
            // WeaponConfig holds the authored number; the body is where the
            // resolve reads it from, because body-to-body needs both masses.
            cur.ship.body.mass = weapons.playerMass;
            // The shut-door faces for this step, built before anything asks a
            // geometric question.
            dvk::sim::buildSolidDoors(level, cur.game, solidDoors);

            dvk::sim::resolveCollision(level, cur.ship.body, collision,
                                       static_cast<float>(kSimStep), &contacts, &solidDoors);

            dvk::sim::FireControls fire;
            // Held triggers belong to every fixed step. Only button edges are
            // consumed once per rendered frame; otherwise a continuous weapon
            // loses damage and energy drain whenever a frame runs two steps.
            fire.primary = dvk::bindingDown(input, bindings,
                                            dvk::PlayerAction::PrimaryFire);
            fire.secondary = dvk::bindingDown(input, bindings,
                                              dvk::PlayerAction::SecondaryFire);
            if (!consumedMouse) {
                fire.flare = pendingFlare;
                fire.selectWeapon = pendingWeaponSelection;
                fire.cycleSecondary = pendingSecondaryCycle;
            }
            if (opts.autofire >= 0) {
                if (opts.autofire >= dvk::sim::WeaponCount) {
                    // Autofire is held by definition. stepCombat owns the flare
                    // cooldown, just as it owns every weapon's fire interval.
                    fire.flare = true;
                } else {
                    if (!consumedMouse) fire.selectWeapon = opts.autofire;
                    fire.primary =
                        dvk::sim::weaponSlot(opts.autofire) == dvk::sim::WeaponSlot::Primary;
                    fire.secondary =
                        dvk::sim::weaponSlot(opts.autofire) == dvk::sim::WeaponSlot::Secondary;
                }
            }
            // In the chase camera the guns fire at what is under the reticle,
            // from an eye placed wherever the mine let it sit last frame.
            if (camera == dvk::CameraMode::Chase) {
                const dvk::ChaseAimRay ray = dvk::chaseAimRay(chaseCamera.distance);
                fire.aimFromEye = true;
                fire.eyeLocal = ray.eye;
                fire.aimLocal = ray.direction;
            }
            dvk::sim::stepCombat(level, cur.combat, cur.ship.body, cur.ship.orientation, weapons,
                                 fire, static_cast<float>(kSimStep), &cur.robots, &solidDoors,
                                 &soundEvents, &cur.props);
            if (!consumedMouse) {
                pendingFlare = false;
                pendingWeaponSelection = -1;
                pendingSecondaryCycle = false;
            }
            // Built before the robots look, because their sight ray is the
            // first thing that consumes it.
            dvk::buildOccluders(level, cur, occluders);

            dvk::sim::stepRobots(level, robotConfig, weapons, cur.robots, cur.combat,
                                 cur.ship.body, static_cast<float>(kSimStep), &solidDoors,
                                 &soundEvents, &cur.props, &occluders);
            dvk::sim::stepProps(level, cur.props, static_cast<float>(kSimStep), &solidDoors);

            // Bodies against each other, once everything has integrated and
            // been put back inside the mine by its own system. This is the only
            // place that sees the ship, the robots and the props at the same
            // time, which body-to-body contact needs: it wants both masses, and
            // it has to interleave with the wall resolve rather than run before
            // or after it. The pass ends on the mine, so nothing it does can
            // leave a body outside the world.
            dvk::buildBodySet(cur, bodies);
            const dvk::sim::SolidObjects solidObjects{cur.combat.objects.alive.data(),
                                                      cur.combat.objects.count};
            bodyStats = dvk::sim::resolveBodies(level, bodies, collision,
                                                static_cast<float>(kSimStep), &solidDoors,
                                                &solidObjects);
            dvk::sim::stepGame(level, gameConfig, cur.game, cur.combat, cur.robots, robotConfig,
                               cur.ship.body, static_cast<float>(kSimStep), &soundEvents,
                               &script.triggers(), &script.timers(), &script.pickupHooks());
            // The moment this step's damage disabled the ship, while it can
            // still repair itself: the transmission is the ship telling the
            // player it is on it. Measured against the step before, so a level
            // that starts the ship disabled, or a script that sets shields
            // between steps, announces nothing. A ship knocked straight to zero
            // is dead, not disabled.
            if (!dvk::sim::playerDisabled(prev.combat.player) &&
                dvk::sim::playerDisabled(cur.combat.player) &&
                cur.combat.player.shields > 0.0f && cur.combat.player.selfRepair) {
                shipDisabled = true;
            }
            // In M5 zero shields respawned you on the spot. From M7 it ends
            // the level, and the end screen offers a restart.
            consumedMouse = true;
            accumulator -= kSimStep;
            ++totalSteps;
            if (cur.game.phase == dvk::sim::GamePhase::Won ||
                cur.game.phase == dvk::sim::GamePhase::Lost) {
                session.finish();
                runFinished = true;
                shell.captureMouse(false);
                accumulator = 0.0;
            }
        }
        // CLAUDE.md says the sim step does not allocate. M8 says to instrument
        // that rather than believe it.
        allocations.recordSim(frame, simAllocs.count());

        // Lua is deliberately outside the measured fixed step. Every step in
        // this rendered frame has already latched its trigger ids into POD
        // state; callbacks may allocate without compromising sim determinism.
        script.drain(cur.game,
                     dvk::ScriptWorld{&cur.combat, &cur.robots, &robotConfig});
        // A callback asking for the ship back. Applied to the live state only:
        // this is something that happened during the run, not a condition the
        // run begins from, and writing it to initial() would carry it across a
        // restart and skip the opening it belongs to. The disabled flag is
        // recomputed from shields at the top of every step, so the ship flies
        // again on the next one with nothing else to do.
        if (const float shields = script.takeShields(); shields >= 0.0f) {
            cur.combat.player.shields = shields;
        }
        // Live state only, for the same reason: a restart goes back to what the
        // loadout said.
        if (const int selfRepair = script.takeSelfRepair(); selfRepair >= 0) {
            cur.combat.player.selfRepair = selfRepair != 0;
        }
        // Before the script's own message, so a story beat landing on the
        // same frame is the one left on screen.
        if (shipDisabled && recoveryClip) comm.play(-1, recoveryMessage, now);
        if (const int requested = script.takeMessage(); requested >= 0) {
            if (const dvk::ScriptMessage* message = script.message(requested)) {
                comm.play(requested, *message, now);
            }
        }
        // A pane stops the simulation, so this has to happen before the
        // accumulator is read for the interpolation below - and the shell's
        // effect is what clears the time the stopped simulation is no longer
        // going to owe anyone.
        if (const int requested = script.takeTutorial(); requested >= 0 && playerSession) {
            if (const dvk::ScriptTutorial* authored = script.tutorial(requested)) {
                const dvk::ShellEffect effect = shell.showTutorial(*authored, now);
                if (effect.resetAccumulator) accumulator = 0.0;
                if (effect.resyncPrevious) prev = cur;
            }
        }

        const dvk::CommFrame commFrame = comm.frame(now);

        // --comm is a fixed screenshot panel with no voice, not an actual
        // transmission. Scripted comms duck both running music transports.
        const bool commDucked = comm.activeMessage() >= 0 && commFrame.visible;
        dvk::updateSoundtrack(musicDirector, audio, cur, session.mode(), commDucked,
                              shell.settings().musicVolume, static_cast<float>(elapsed),
                              musicMarqueeTime);

        if (runFinished) shell.finishRun(cur.game, playerSession);

        const auto alpha = static_cast<float>(accumulator / kSimStep);
        dvk::FramePrepareContext frameContext{
            prev, cur, alpha, camera, shell.settings(), clock.seconds(), elapsed, audio,
            soundEvents, level, flight, weapons, robotConfig, flood, lightDoorSignature,
            dynamicLights,
            solidDoors, occluders, playerLight, playerSeen, aim, chaseCamera};
        dvk::PreparedFrame prepared = dvk::prepareRenderFrame(frameContext);
        dvk::RenderState& rs = prepared.renderState;
        const glm::vec3& shipForward = prepared.shipForward;
        const dvk::sim::RayHit& noseRay = prepared.noseRay;

        const glm::ivec2 fb = window.framebufferSize();
        dvk::alloc::Scope drawAllocs;
        renderer.beginFrame(fb.x, fb.y);
        renderer.buildDebugLines(level, rs, contacts, noseRay);
        if (!freezeVisible) {
            // The walk starts where the eye is. In chase view that is behind
            // and above the ship and often a segment back from it; starting at
            // the ship's segment clipped everything around the eye to the one
            // portal between the two, so rooms still on screen dropped out.
            // The eye sits on a clear line out from the ship, so following
            // portal planes from the ship's segment finds its segment.
            const glm::vec3 eye = renderer.eyePos(rs);
            const int eyeSegment = dvk::trackSegment(level, eye, rs.shipSegment);
            walk.build(level, eyeSegment, renderer.viewProj(rs), eye, &cur.game.doorField);
        }

        const dvk::HudBuildContext hudContext{
            cur,
            rs,
            weapons,
            robotConfig,
            bindings,
            static_cast<float>(kSimStep),
            !opts.noUi && session.active() && session.mode() != dvk::AppMode::RunSummary,
            aim.point,
            aim.onBreakable,
            !automap.open,
            shell.settings().leadMarker,
            playerSeen,
            commFrame,
            comm.activeClip(),
            script.message(comm.activeMessage()),
            audio,
            musicMarqueeTime};
        const dvk::gl::HudData hud = dvk::buildHud(hudContext, hudScratch);

        dvk::MenuView menuView = shell.menu().view(fb.x, fb.y);
        // The pane's picture runs on the application clock, which the menu has
        // no access to, so the frame it should be showing is filled in here.
        // Deliberately after view(): everything else about the screen is a pure
        // function of the menu's own state, and this is the one part that is
        // a function of the time.
        if (menuView.screen == dvk::MenuScreen::Tutorial) {
            const dvk::TutorialFrame pane = tutorial.frame(now);
            menuView.tutorialClip = tutorial.clip();
            menuView.tutorialFrame = pane.frame;
            menuView.tutorialAlpha = pane.alpha;
        }
        if (opts.noUi) menuView.screen = dvk::MenuScreen::Hidden;

        if (automap.open) {
            dvk::buildAutomap(automap, level, cur, rs, shipForward, robotConfig, opts.reveal);
            renderer.drawAutomap(automap, level, hud, menuView);
        } else {
            renderer.drawScene(rs, level, walk, dynamicLights, weapons.shadowBudget, hud,
                               menuView);
        }
        renderer.resolve();
        // The same question for the draw path: everything from beginFrame to
        // resolve, which is where per-frame geometry is built and submitted.
        allocations.recordDraw(frame, drawAllocs.count());

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        // --no-ui still runs a frame, so ImGui's internal state stays
        // consistent; it just does not open any window.
        // ImGui is debug-only from M7: the HUD carries what the player needs.
        if (showDebugUi) {
            dvk::gl::DebugUiContext debugContext{
                frameMs, totalSteps, kSimHz, alpha, fb, renderer, rs, level, walk,
                freezeVisible, flight, flightDefaults, camera, input, cur, contacts, bodies,
                bodyStats, noseRay, frame, dvk::AllocationTally::kWarmupFrames,
                allocations.worstSim, allocations.simFrames, allocations.worstDraw,
                allocations.drawFrames, playerLight, playerSeen, weapons, robotConfig,
                audio, opts, flood, levelFile, lightEditorSelection, kFlightConfigPath,
                kWeaponConfigPath};
            dvk::gl::drawDebugUi(debugContext);
        }
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        ++frame;
        const bool lastFrame = (opts.frames >= 0 && frame >= opts.frames) ||
                               (opts.levelSwitchTest && frame >= dvk::LevelSwitchTest::kEndFrame);
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

    const double elapsed = (loopEnd > 0.0 ? loopEnd : clock.seconds()) - loopStart -
                           minimizedSeconds - suspendedSeconds;
    dvk::reportStepRate(frame, totalSteps, elapsed, kSimHz);
    if (opts.resizeTest) dvk::reportMinimized(minimizedFrames, minimizedSeconds);
    dvk::reportSound(audio, opts.noSound);
    dvk::reportGpu(renderer.timers());

    int exitCode = 0;
    if (opts.levelSwitchTest) {
        exitCode = switchTest.report(levelFile, level.segmentCount(),
                                     session.mode() == dvk::AppMode::Playing,
                                     cur.ship.body.segment, totalSteps);
    }
    allocations.report(frame);
    return exitCode;
}
