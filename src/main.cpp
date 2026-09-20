#include "audio/audio.h"
#include "audio/sound_test.h"
#include "core/input.h"
#include "core/log.h"
#include "core/options.h"
#include "core/time.h"
#include "core/window.h"
#include "game/asset_check.h"
#include "game/bindings.h"
#include "game/hud_slots.h"
#include "game/level.h"
#include "game/level_list.h"
#include "game/state.h"
#include "game/visibility.h"
#include "game/collision_test.h"
#include "game/light_test.h"
#include "game/loop_test.h"
#include "game/music.h"
#include "game/progress.h"
#include "game/robots_test.h"
#include "game/comm_player.h"
#include "game/session.h"
#include "game/script.h"
#include "game/settings.h"
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
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

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
constexpr const char* kWeaponConfigPath = "assets/config/weapons.json";
constexpr const char* kRobotConfigPath = "assets/config/robots.json";
constexpr const char* kSoundDir = "assets/sounds";
constexpr const char* kCommRoot = "assets/comm";
constexpr const char* kMusicDir = "assets/music";

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

// --level-switch-test. Loading a second mine touches GL resources, the session
// and every per-level cache in the frame loop, so the only honest check is to
// do it and then keep playing: move the cursor down one row, start that level,
// and fly it for long enough that the new mine is simulated and drawn rather
// than merely loaded.
constexpr int kSwitchPickFrame = 10;
constexpr int kSwitchConfirmFrame = 20;
constexpr int kSwitchEndFrame = 90;

} // namespace

int main(int argc, char** argv) {
    const dvk::Options opts = dvk::Options::parse(argc, argv);
    const char* levelPath = opts.level.empty() ? kDefaultLevelPath : opts.level.c_str();

    if (opts.settingsTest) return dvk::runSettingsTest();
    if (opts.progressTest) return dvk::runProgressTest();
    if (opts.musicTest) return dvk::runMusicTest();
    if (opts.commTest) {
        const int clip = dvk::gl::runCommTest(kCommRoot);
        const int player = dvk::runCommPlayerTest();
        return clip != 0 || player != 0 ? 1 : 0;
    }
    if (opts.scriptTest) return dvk::runScriptTest(opts.level);


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
        return dvk::audio::runSoundTest(kSoundDir, kMusicDir);
    }

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
    dvk::PlayerProgress progress = dvk::loadPlayerProgress(progressPath);
    if (opts.volumeOverride) settings.masterVolume = opts.volume;
    if (opts.sizeOverride) {
        settings.windowWidth = opts.width;
        settings.windowHeight = opts.height;
        settings.fullscreen = false;
    }

    dvk::Window::Desc desc;
    desc.width = settings.windowWidth;
    desc.height = settings.windowHeight;
#ifdef DVK_DEBUG
    desc.debugContext = true;
#endif
    // A frame-limited run must not be pinned to the refresh rate, except under
    // --resize-test, where the window manager needs real time to deliver
    // iconify and restore.
    desc.vsync = settings.vsync && (opts.frames < 0 || opts.resizeTest);
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
    const dvk::sim::RobotConfig robotConfig = dvk::sim::loadRobotConfig(kRobotConfigPath);
    dvk::gl::Renderer renderer(level, robotConfig);

    dvk::ScriptHost script;

    const auto loadScript = [&] {
        script.load(level);
        for (const dvk::ScriptMessage& message : script.messages()) {
            if (renderer.commClipIndex(message.clip) < 0) {
                dvk::log::fatal("Lua message '{}' names unknown comm clip '{}'", message.id,
                                message.clip);
            }
        }
    };
    loadScript();
    if (!opts.comm.empty() && renderer.commClipIndex(opts.comm) < 0) {
        dvk::log::fatal("--comm names unknown clip '{}'", opts.comm);
    }

    dvk::Clock clock;
    // Sound. A container with no sound card is the ordinary case for this and
    // is a warning, not an error: the mixer still runs, nothing pulls from it,
    // and every other number the run reports is unaffected. Declared here
    // rather than at its init below because starting a transmission starts its
    // voice, and that is decided a few lines down.
    dvk::audio::Audio audio;

    dvk::CommPlayer commPlayer;
    int activeCommMessage = -1;
    // Which clip the panel is drawing. Every frame index the player produces is
    // relative to this one, so the two have to be set together or a message
    // ends up playing another message's pictures.
    int activeCommClip = 0;
    const auto commTiming = [&](int clip) {
        const dvk::gl::CommClipInfo* info = renderer.commClip(clip);
        if (!info) return dvk::CommClipTiming{};
        return dvk::CommClipTiming{info->frameCount, info->framesPerSecond, info->displaySeconds};
    };
    // Names are validated at load, so an unknown one here means the library
    // changed underneath us; clip 0 keeps the panel showing something rather
    // than indexing off the end of the array.
    const auto selectCommClip = [&](std::string_view name) {
        return std::max(0, renderer.commClipIndex(name));
    };
    // Starts the picture and, if the clip has one, the voice beside it. They
    // are started together and never corrected against each other afterwards;
    // see audio.h and docs/ARCHITECTURE.md for why that is enough.
    const auto startCommClip = [&](std::string_view name, float seconds) {
        activeCommClip = selectCommClip(name);
        dvk::CommClipTiming timing = commTiming(activeCommClip);
        if (seconds > 0.0f) timing.displaySeconds = seconds;
        commPlayer.configure(timing);
        commPlayer.start(clock.seconds());
        audio.playComm(audio.bank().commClip(name));
    };
    // Entering a level shows nothing. A transmission is level content: the only
    // thing that raises one is a script calling comm(), so a level with no
    // script has none, and a level that wants one says when. This also runs on
    // restart, where it is what clears a transmission the previous attempt was
    // part way through.
    const auto startComm = [&]() {
        activeCommMessage = -1;
        activeCommClip = 0;
        if (!opts.comm.empty()) {
            // --comm is a screenshot pin, not a performance: a fixed frame and
            // no voice, so the picture is the same every run. The only thing
            // that puts a transmission on screen without a script.
            activeCommClip = selectCommClip(opts.comm);
            commPlayer.configure(commTiming(activeCommClip));
            commPlayer.hold(1.0);
            audio.stopComm();
            return;
        }
        commPlayer.stop();
        audio.stopComm();
    };

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
    // What the level's script says the player starts with. On `initial()` for
    // the same reason the headlight below is: that is the state every start
    // and every restart is copied from, and the live states are only written
    // here as well so a run that never calls start() - a screenshot, a
    // frame-limited pass - still shows the loadout the level asked for.
    const auto applySessionLoadout = [&] {
        dvk::applyLoadout(script.loadout(), session.initial().combat.player);
        dvk::applyLoadout(script.loadout(), prev.combat.player);
        dvk::applyLoadout(script.loadout(), cur.combat.player);
    };
    applySessionLoadout();
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
    if (opts.targetHud && session.initial().robots.count > 0) {
        dvk::sim::Robot& target = session.initial().robots.robots[0];
        const dvk::sim::RobotDef& def =
            robotConfig.types[static_cast<size_t>(target.type)];
        target.health = def.health * 0.09f;
        target.mode = dvk::sim::RobotMode::Disabled;
        session.initial().combat.player.lastTargetRobot = 0;
        session.initial().combat.player.targetHudTime = dvk::sim::kTargetHudTime;
    }
    dvk::PlayerMenu menu;
    std::vector<dvk::LevelEntry> levelEntries = dvk::discoverLevels(kLevelDir, levelFile);
    for (dvk::LevelEntry& entry : levelEntries) {
        if (const dvk::LevelRecord* record =
                progress.find(dvk::levelProgressId(entry.path))) {
            entry.completions = record->wins;
            entry.bestSeconds = static_cast<int>(record->bestTime);
        }
    }
    menu.setLevels(std::move(levelEntries));
    // The list opens on the level that is loaded, so confirming twice from the
    // main menu starts what a plain run has always started.
    for (int i = 0; i < static_cast<int>(menu.levels().size()); ++i) {
        if (menu.levels()[static_cast<size_t>(i)].path == levelFile) menu.setSelectedLevel(i);
    }

    const bool commandLineRun =
        argc > 1 && !opts.startMenu && !opts.levelSelect && !opts.levelSwitchTest &&
        !opts.settingsMenu && !opts.controlsMenu;
    const bool recordsProgress = !commandLineRun && !opts.levelSwitchTest && opts.frames < 0 &&
                                 opts.screenshot.empty() && !opts.resizeTest;
    if (commandLineRun) {
        session.start();
        startComm();
        menu.hide();
    } else if (opts.settingsMenu) {
        menu.showMain();
        menu.showSettings(settings);
    } else if (opts.controlsMenu) {
        menu.showMain();
        menu.showSettings(settings);
        menu.showControls();
    } else if (opts.levelSelect || opts.levelSwitchTest) {
        menu.showLevelSelect();
    } else {
        menu.showMain();
    }

    dvk::sim::ContactLog contacts;
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
    audio.setMasterGain(settings.masterVolume);
    audio.setMuted(settings.muted);
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
    dvk::gl::AutomapData automap;
    automap.open = opts.automap;
    // Where the wheel has asked the map to be. `automap.zoom` chases it rather
    // than snapping, so a click of the wheel reads as a move in and not as a
    // cut.
    float automapZoomTarget = automap.zoom;

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
    // Loading another mine, from the level list and from nowhere else. Every
    // piece of state that is a function of which level is loaded is rebuilt
    // here: the renderer's level resources, the session's initial and live
    // states, and the caches in this loop that index segments, lights and
    // contacts of the mine that was loaded a moment ago. Anything left stale
    // here shows up as a light in a wall or a ship in the rock, so the resets
    // stay together rather than spread through the menu handlers.
    const auto loadLevelFile = [&](const std::string& path) {
        if (path == levelFile) return;
        dvk::log::info("switching to level '{}'", path);
        level = dvk::loadLevel(path);   // fatal if the file is broken, as everywhere else
        levelFile = path;

        renderer.setLevel(level);
        session.load(level, robotConfig, opts.startSegment);
        loadScript();
        applySessionLoadout();
        activeCommMessage = -1;
        activeCommClip = 0;
        commPlayer.stop();
        audio.stopComm();
        if (opts.headlight) {
            session.initial().combat.player.headlightOn = true;
            prev.combat.player.headlightOn = true;
            cur.combat.player.headlightOn = true;
        }

        contacts.clear();
        const bool wasOpen = automap.open;
        automap = dvk::gl::AutomapData{};
        automap.open = wasOpen;
        automapZoomTarget = automap.zoom;
        // The level's lights were flooded at load with no doors; -1 makes the
        // loop below rebuild them against this level's door state.
        lightDoorSignature = -1;
        lightEditorSelection = -1;
    };

    const std::string switchTestFrom = levelFile;
    const int switchTestFromSegments = level.segmentCount();

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
    // How far down the firing line the reticle is drawn. Held across frames
    // because it is chased rather than set: see where it is updated below.
    float aimDistance = dvk::kDefaultAimDistance;
    // Whether the soundtrack is in combat. Held across frames because the
    // fight being over is a thing that has to last: see where it is updated
    // below.
    dvk::MusicDirector musicDirector;
    // The HUD marquee's own clock. Real time rather than simulation time: the
    // transports keep playing through a pause, so the scroll keeps moving with
    // them. Reset whenever the soundtrack stops, so the next level's readout
    // starts at the left rather than wherever the last one had got to.
    float musicMarqueeTime = 0.0f;
    // The skip key's name for the HUD hint, formatted into a buffer that
    // outlives the frame rather than into a string built in the draw path.
    // Refilled every frame because the binding can change under the player in
    // the settings menu without the level ever being left.
    char musicNextKeyLabel[32] = {};
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
    dvk::PlayerSettings settingsBeforeDisplayChange = settings;
    bool displayConfirmationPending = false;
    double displayConfirmationDeadline = 0.0;

    const auto applyRuntimeSettings = [&] {
        audio.setMasterGain(settings.masterVolume);
        audio.setMuted(settings.muted);
        window.setVsync(settings.vsync && (opts.frames < 0 || opts.resizeTest));
        window.setDisplay(settings.fullscreen, settings.windowWidth, settings.windowHeight);
    };

    const auto finishDisplayConfirmation = [&](bool keep) {
        if (!keep) {
            settings.fullscreen = settingsBeforeDisplayChange.fullscreen;
            settings.windowWidth = settingsBeforeDisplayChange.windowWidth;
            settings.windowHeight = settingsBeforeDisplayChange.windowHeight;
            settings.vsync = settingsBeforeDisplayChange.vsync;
            applyRuntimeSettings();
        }
        dvk::savePlayerSettings(settingsPath, settings);
        displayConfirmationPending = false;
        menu.closeSettings();
    };

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

        if (displayConfirmationPending) {
            const int seconds = std::max(0, static_cast<int>(std::ceil(
                                                displayConfirmationDeadline - now)));
            menu.setConfirmationSeconds(seconds);
            if (now >= displayConfirmationDeadline) finishDisplayConfirmation(false);
        }

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

        const bool pausePressed =
            dvk::bindingPressed(input, settings.bindings, dvk::PlayerAction::Pause);
        const bool menuEscape = input.pressed(GLFW_KEY_ESCAPE);
        const bool bindingCaptureActive =
            menu.screen() == dvk::MenuScreen::Controls && menu.capturingBinding();
        if (!bindingCaptureActive && (pausePressed || menuEscape) &&
            menu.screen() == dvk::MenuScreen::DisplayConfirmation) {
            finishDisplayConfirmation(false);
            accumulator = 0.0;
        } else if (!bindingCaptureActive && (pausePressed || menuEscape) &&
                   menu.screen() == dvk::MenuScreen::Controls) {
            menu.closeControls();
            accumulator = 0.0;
        } else if (!bindingCaptureActive && (pausePressed || menuEscape) &&
                   menu.screen() == dvk::MenuScreen::Settings) {
            menu.closeSettings();
            accumulator = 0.0;
        } else if (!bindingCaptureActive && pausePressed) {
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
        dvk::MenuInput menuInput = dvk::readMenuInput(input, input.cursorPosition() * pointerScale);
        // The wheel scrolls the two lists that are longer than the screen, and
        // is taken only while one of them is up: the automap and the debug
        // panels want it the rest of the time.
        if (menu.screen() == dvk::MenuScreen::LevelSelect ||
            menu.screen() == dvk::MenuScreen::Controls) {
            menuInput.scroll = input.consumeScroll();
        }
        if (opts.levelSwitchTest) {
            // The script drives the cursor, so the pointer is parked off every
            // button: under a window manager the mouse sits wherever it was
            // left, and hovering a row would decide which level this test picks.
            menuInput = dvk::MenuInput{};
            menuInput.pointer = glm::vec2(-1.0f);
            menuInput.next = frame == kSwitchPickFrame;
            menuInput.confirm = frame == kSwitchConfirmFrame;
        }
        const dvk::MenuAction menuAction =
            menu.update(menuInput, menuFramebuffer.x, menuFramebuffer.y);
        switch (menuAction) {
            case dvk::MenuAction::ChooseLevel: menu.showLevelSelect(); break;
            case dvk::MenuAction::OpenSettings: menu.showSettings(settings); break;
            case dvk::MenuAction::OpenControls: menu.showControls(); break;
            case dvk::MenuAction::ApplySettings: {
                const dvk::PlayerSettings previous = settings;
                settings = menu.editedSettings();
                const bool displayChanged =
                    settings.fullscreen != previous.fullscreen ||
                    settings.windowWidth != previous.windowWidth ||
                    settings.windowHeight != previous.windowHeight || settings.vsync != previous.vsync;
                settingsBeforeDisplayChange = previous;
                applyRuntimeSettings();
                if (displayChanged) {
                    displayConfirmationPending = true;
                    displayConfirmationDeadline = now + 15.0;
                    menu.showDisplayConfirmation(15);
                } else {
                    dvk::savePlayerSettings(settingsPath, settings);
                    menu.closeSettings();
                }
                accumulator = 0.0;
                break;
            }
            case dvk::MenuAction::KeepDisplay: finishDisplayConfirmation(true); break;
            case dvk::MenuAction::RevertDisplay: finishDisplayConfirmation(false); break;
            case dvk::MenuAction::Back:
                if (menu.screen() == dvk::MenuScreen::Controls) menu.closeControls();
                else if (menu.screen() == dvk::MenuScreen::Settings) menu.closeSettings();
                else menu.showMain();
                break;
            case dvk::MenuAction::Start:
                if (menu.selectedLevel() >= 0) {
                    loadLevelFile(menu.levels()[static_cast<size_t>(menu.selectedLevel())].path);
                }
                loadScript();
                session.start();
                startComm();
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
                loadScript();
                session.restart();
                startComm();
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

        if (!bindingCaptureActive && input.pressed(GLFW_KEY_F3)) {
            showDebugUi = !showDebugUi;
        } else if (session.mode() == dvk::AppMode::Playing &&
                   dvk::bindingPressed(input, settings.bindings,
                                       dvk::PlayerAction::Headlight)) {
            // Refuses to come on with no energy left, which is the whole point
            // of it costing any.
            cur.combat.player.headlightOn =
                !cur.combat.player.headlightOn && cur.combat.player.energy > 0.0f;
        }
        if (!bindingCaptureActive &&
            dvk::bindingPressed(input, settings.bindings, dvk::PlayerAction::ChaseCamera)) {
            camera = camera == dvk::CameraMode::FirstPerson ? dvk::CameraMode::Chase
                                                            : dvk::CameraMode::FirstPerson;
        }
        if (!bindingCaptureActive && session.mode() == dvk::AppMode::Playing &&
            dvk::bindingPressed(input, settings.bindings, dvk::PlayerAction::Automap)) {
            automap.open = !automap.open;
        }
        if (!bindingCaptureActive &&
            dvk::bindingPressed(input, settings.bindings, dvk::PlayerAction::NextTrack)) {
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
            dvk::bindingPressed(input, settings.bindings, dvk::PlayerAction::Mute)) {
            // Mute keeps the device open and the ambient channels running, so
            // unmuting is instant and does not restart the engine mid-note.
            audio.setMuted(!audio.muted());
            settings.muted = audio.muted();
            dvk::savePlayerSettings(settingsPath, settings);
        }

        if (!session.advancesSimulation()) {
            accumulator = 0.0;
            pendingWeaponSelection = -1;
            pendingSecondaryCycle = false;
            pendingFlare = false;
        } else if (!bindingCaptureActive) {
            // A fast render frame can have no fixed simulation step. Preserve
            // one-shot gameplay input until a step actually consumes it.
            pendingFlare = pendingFlare ||
                           dvk::bindingPressed(input, settings.bindings,
                                               dvk::PlayerAction::Flare);
            // A key each for the four primaries; the missiles share one that
            // cycles them, which the simulation resolves against whichever one
            // is selected at the time.
            for (const dvk::WeaponSelectAction& select : dvk::kWeaponSelectActions) {
                if (dvk::bindingPressed(input, settings.bindings, select.action)) {
                    pendingWeaponSelection = select.weapon;
                }
            }
            pendingSecondaryCycle =
                pendingSecondaryCycle ||
                dvk::bindingPressed(input, settings.bindings,
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
        soundEvents.clear();
        while (session.advancesSimulation() && accumulator >= kSimStep) {
            prev = cur;
            // The whole frame's motion belongs to the first step; later steps
            // in the same frame have no new input to apply.
            if (!consumedMouse && !automap.open) {
                mouseDelta = input.consumeMouseDelta() * settings.mouseSensitivity;
                if (settings.invertMouseY) mouseDelta.y = -mouseDelta.y;
            }
            dvk::sim::ShipControls controls =
                dvk::readControls(input, consumedMouse ? glm::vec2(0.0f) : mouseDelta,
                                  settings.bindings);
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
            fire.primary = dvk::bindingDown(input, settings.bindings,
                                            dvk::PlayerAction::PrimaryFire);
            fire.secondary = dvk::bindingDown(input, settings.bindings,
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
            dvk::sim::stepCombat(level, cur.combat, cur.ship.body, cur.ship.orientation, weapons,
                                 fire, static_cast<float>(kSimStep), &cur.robots, &solidDoors,
                                 &soundEvents, &cur.props);
            if (!consumedMouse) {
                pendingFlare = false;
                pendingWeaponSelection = -1;
                pendingSecondaryCycle = false;
            }
            // Built before the robots look, because their sight ray is the
            // first thing that consumes it. Robots and props go in as spheres
            // of their own body radius rather than their model bounds, so what
            // blocks a line of sight is the same size as what stops a bullet.
            occluders.clear();
            occluders.addBody(cur.ship.body.position, cur.ship.body.radius,
                              {dvk::sim::OccluderKind::Ship, -1}, cur.ship.body.segment);
            for (int i = 0; i < cur.robots.count; ++i) {
                const dvk::sim::Robot& robot = cur.robots.robots[static_cast<size_t>(i)];
                if (!robot.alive) continue;
                occluders.addBody(robot.body.position, robot.body.radius,
                                  {dvk::sim::OccluderKind::Robot, i}, robot.body.segment);
            }
            for (int i = 0; i < cur.props.floatingCount; ++i) {
                const dvk::sim::FloatingLight& ball = cur.props.floating[static_cast<size_t>(i)];
                if (!ball.alive) continue;
                occluders.addBody(ball.body.position, ball.body.radius,
                                  {dvk::sim::OccluderKind::Prop, i}, ball.body.segment);
            }
            // Pillars and crates, on the same `alive` flags that decide whether
            // they stop a bullet: shoot a crate away and it stops casting too.
            for (int i = 0; i < cur.combat.objects.count &&
                            static_cast<size_t>(i) < level.objectProxies.size(); ++i) {
                if (!cur.combat.objects.alive[static_cast<size_t>(i)]) continue;
                occluders.add(level.objectProxies[static_cast<size_t>(i)],
                              {dvk::sim::OccluderKind::Object, i},
                              level.objects[static_cast<size_t>(i)].segment);
            }

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
            bodies.clear();
            bodies.add(cur.ship.body);
            for (int i = 0; i < cur.robots.count; ++i) {
                dvk::sim::Robot& robot = cur.robots.robots[static_cast<size_t>(i)];
                if (robot.alive) bodies.add(robot.body);
            }
            for (int i = 0; i < cur.props.floatingCount; ++i) {
                dvk::sim::FloatingLight& ball = cur.props.floating[static_cast<size_t>(i)];
                if (ball.alive) bodies.add(ball.body, ball.restitution);
            }
            const dvk::sim::SolidObjects solidObjects{cur.combat.objects.alive.data(),
                                                      cur.combat.objects.count};
            bodyStats = dvk::sim::resolveBodies(level, bodies, collision,
                                                static_cast<float>(kSimStep), &solidDoors,
                                                &solidObjects);
            dvk::sim::stepGame(level, gameConfig, cur.game, cur.combat, cur.robots, robotConfig,
                               cur.ship.body, static_cast<float>(kSimStep), &soundEvents,
                               &script.triggers(), &script.timers());
            // In M5 zero shields respawned you on the spot. From M7 it ends
            // the level, and the end screen offers a restart.
            consumedMouse = true;
            accumulator -= kSimStep;
            ++totalSteps;
            if (cur.game.phase == dvk::sim::GamePhase::Won ||
                cur.game.phase == dvk::sim::GamePhase::Lost) {
                session.finish();
                runFinished = true;
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
        // Lua is deliberately outside the measured fixed step. Every step in
        // this rendered frame has already latched its trigger ids into POD
        // state; callbacks may allocate without compromising sim determinism.
        script.drain(cur.game);
        // A callback asking for the ship back. Applied to the live state only:
        // this is something that happened during the run, not a condition the
        // run begins from, and writing it to initial() would carry it across a
        // restart and skip the opening it belongs to. The disabled flag is
        // recomputed from shields at the top of every step, so the ship flies
        // again on the next one with nothing else to do.
        if (const float shields = script.takeShields(); shields >= 0.0f) {
            cur.combat.player.shields = shields;
        }
        if (const int requested = script.takeMessage(); requested >= 0) {
            if (const dvk::ScriptMessage* message = script.message(requested)) {
                activeCommMessage = requested;
                // The clip's own displaySeconds is how long the pictures run;
                // the message says how long it is on screen, and the frames
                // loop to fill it.
                startCommClip(message->clip, message->seconds);
            }
        }

        const dvk::CommFrame commFrame = commPlayer.at(now);

        // Adaptive music is presentation state, derived from the simulation
        // rather than fed back into it. Awareness already carries the useful
        // hysteresis: it snaps to one on contact and takes about three seconds
        // to decay after the player breaks line of sight, which is exactly as
        // long as the soundtrack should go on believing there is a fight.
        float combatAwareness = 0.0f;
        for (int i = 0; i < cur.robots.count; ++i) {
            const dvk::sim::Robot& robot = cur.robots.robots[static_cast<size_t>(i)];
            if (!robot.alive || robot.mode == dvk::sim::RobotMode::Disabled) continue;
            combatAwareness = std::max(combatAwareness, robot.awareness);
        }
        const bool forcedCombat = cur.combat.player.hurtTimer > 0.0f ||
                                  cur.game.phase == dvk::sim::GamePhase::Countdown;

        // Frozen while paused: a pause is not the fight being over, and this is
        // the same delta the envelopes inside setMusicState run on, so the
        // crossfade stops with it.
        const float musicDelta =
            session.mode() == dvk::AppMode::Playing ? static_cast<float>(elapsed) : 0.0f;
        const float combatTarget = musicDirector.update(combatAwareness, forcedCombat, musicDelta);

        const bool musicActive = session.mode() == dvk::AppMode::Playing ||
                                 session.mode() == dvk::AppMode::Paused;
        if (musicActive) {
            // --comm is a fixed screenshot panel with no voice, not an actual
            // transmission. Scripted comms duck both running music transports.
            const bool commDucked = activeCommMessage >= 0 && commFrame.visible;
            audio.setMusicState(combatTarget, settings.musicVolume, commDucked, musicDelta);
            musicMarqueeTime += static_cast<float>(elapsed);
        } else {
            audio.stopMusic();
            musicMarqueeTime = 0.0f;
            // The next level starts its own fight. Carrying a grace across the
            // gap would open it mid-crossfade out of a mine nobody is in.
            musicDirector = dvk::MusicDirector{};
        }

        if (runFinished) {
            const char* recordNote = "";
            if (recordsProgress) {
                const std::string levelId = dvk::levelProgressId(levelFile);
                const dvk::RunRecordUpdate update =
                    dvk::recordRun(progress, levelId, cur.game.phase == dvk::sim::GamePhase::Won,
                                   cur.game.elapsed, cur.game.robotsDestroyed);
                dvk::savePlayerProgress(progressPath, progress);
                if (const dvk::LevelRecord* record = progress.find(levelId)) {
                    menu.setLevelRecord(levelFile, record->wins,
                                        static_cast<int>(record->bestTime));
                }
                recordNote = dvk::runRecordNote(update);
            }
            menu.showSummary(cur.game.phase == dvk::sim::GamePhase::Won, cur.game.endReason,
                             cur.game.elapsed, cur.game.robotsDestroyed, cur.game.robotsAtStart,
                             recordNote);
        }

        const auto alpha = static_cast<float>(accumulator / kSimStep);
        dvk::FramePrepareContext frameContext{
            prev, cur, alpha, camera, settings, clock.seconds(), elapsed, audio, soundEvents,
            level, flight, weapons, flood, lightDoorSignature, dynamicLights, solidDoors,
            occluders, playerLight, playerSeen, aimDistance};
        dvk::PreparedFrame prepared = dvk::prepareRenderFrame(frameContext);
        dvk::RenderState& rs = prepared.renderState;
        const glm::vec3& shipForward = prepared.shipForward;
        const dvk::sim::RayHit& noseRay = prepared.noseRay;

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
        hud.flares = cur.combat.player.flares;
        dvk::fillWeaponSlots(cur.combat.player, weapons, settings.bindings,
                             static_cast<float>(kSimStep), hud);
        for (int k = 0; k < 3; ++k) hud.keys[k] = cur.game.keys[static_cast<size_t>(k)];
        hud.countdown = cur.game.phase == dvk::sim::GamePhase::Countdown ? cur.game.countdown
                                                                        : -1.0f;
        hud.phase = static_cast<int>(cur.game.phase);
        hud.message = cur.game.endReason;
        hud.elapsed = cur.game.elapsed;
        hud.robotsDestroyed = cur.game.robotsDestroyed;
        const int targetSlot = cur.combat.player.lastTargetRobot;
        if (cur.combat.player.targetHudTime > 0.0f && targetSlot >= 0 &&
            targetSlot < cur.robots.count) {
            const dvk::sim::Robot& target =
                cur.robots.robots[static_cast<size_t>(targetSlot)];
            if (target.alive && target.type >= 0 &&
                static_cast<size_t>(target.type) < robotConfig.types.size()) {
                const dvk::sim::RobotDef& def =
                    robotConfig.types[static_cast<size_t>(target.type)];
                hud.hasTarget = true;
                hud.targetDisabled = target.mode == dvk::sim::RobotMode::Disabled;
                hud.targetName = def.name.c_str();
                hud.targetStatus = hud.targetDisabled
                                       ? "engines disabled"
                                       : dvk::sim::kRobotModeNames[static_cast<size_t>(target.mode)];
                hud.targetHealth01 = def.health > 0.0f ? target.health / def.health : 0.0f;
                hud.targetEnergy01 = def.energy > 0.0f ? target.energy / def.energy : 1.0f;
                hud.targetFade = std::min(1.0f, cur.combat.player.targetHudTime / 0.5f);
            }
        }
        hud.commActive = commFrame.visible;
        hud.commFrame = commFrame.frame;
        hud.commAlpha = commFrame.alpha;
        hud.commClip = activeCommClip;
        hud.commSpeaker = "incoming transmission";
        hud.commText = "";
        if (const dvk::ScriptMessage* message = script.message(activeCommMessage)) {
            hud.commSpeaker = message->speaker.c_str();
            hud.commText = message->text.c_str();
        }
        // Read off the crossfade rather than off the director's target: for the
        // two seconds between them the target has already changed and both
        // tracks are still audible, which is exactly what the middle state of
        // the readout is for.
        const dvk::MusicReadout musicHud =
            dvk::musicReadout(audio.musicMix(), audio.musicPlaying(),
                              audio.musicTrackName(0).c_str(),
                              audio.musicTrackName(1).c_str());
        dvk::formatInputBinding(settings.bindings[dvk::PlayerAction::NextTrack],
                                musicNextKeyLabel, sizeof(musicNextKeyLabel));
        hud.musicShow = musicHud.show;
        hud.musicTransitioning = musicHud.transitioning;
        hud.musicTrack = musicHud.text;
        hud.musicNextKey = musicNextKeyLabel;
        hud.musicTime = musicMarqueeTime;
        hud.damageFlash = cur.combat.player.hurtTimer / dvk::sim::kHurtFlashTime;
        // Only the scene draws a firing line; the automap has its own camera.
        hud.aimPoint = rs.shipPos + shipForward * aimDistance;
        hud.hasAimPoint = !automap.open;
        hud.shipDisabled = cur.ship.disabled;
        hud.shipDisabledTime = cur.game.elapsed;
        hud.energyDepleted = dvk::sim::playerEnergyDepleted(cur.combat.player);
        // Both the bar and the word come from `playerSeen` rather than from
        // `playerLight`, even though the latter is the better-behaved number.
        // They have to agree: a bar sitting under its own threshold tick beside
        // the word "exposed" reads as a bug, and the two queries genuinely can
        // disagree - one samples the hull and the other a point. The one worth
        // showing is the one the robots act on.
        hud.illumination = playerSeen.illumination;
        hud.darkThreshold = dvk::sim::kDarkThreshold;
        hud.seen = playerSeen.visible;
        // The headlight is what gave them away only when nothing else had:
        // exactly the case where `visible` would have been false without it.
        hud.seenByHeadlight = playerSeen.headlight &&
                              playerSeen.illumination < dvk::sim::kDarkThreshold;
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
            dvk::gl::DebugUiContext debugContext{
                frameMs, totalSteps, kSimHz, alpha, fb, renderer, rs, level, walk,
                freezeVisible, flight, flightDefaults, camera, input, cur, contacts, bodies,
                bodyStats, noseRay, frame, kAllocWarmupFrames, worstSimAllocs, simAllocFrames,
                worstDrawAllocs, drawAllocFrames, playerLight, playerSeen, weapons, robotConfig,
                audio, opts, flood, levelFile, lightEditorSelection, kFlightConfigPath,
                kWeaponConfigPath};
            dvk::gl::drawDebugUi(debugContext);
        }
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        ++frame;
        const bool lastFrame = (opts.frames >= 0 && frame >= opts.frames) ||
                               (opts.levelSwitchTest && frame >= kSwitchEndFrame);
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

    int exitCode = 0;
    if (opts.levelSwitchTest) {
        const bool changed = levelFile != switchTestFrom;
        const bool playing = session.mode() == dvk::AppMode::Playing;
        const bool inside =
            cur.ship.body.segment >= 0 && cur.ship.body.segment < level.segmentCount();
        const bool stepped = totalSteps > 0;
        dvk::log::info(
            "level-switch-test: started on '{}' ({} segments), ended on '{}' ({} segments); "
            "playing {}, ship in segment {} of {}, {} sim steps in the new mine",
            switchTestFrom, switchTestFromSegments, levelFile, level.segmentCount(),
            playing ? "yes" : "no", cur.ship.body.segment, level.segmentCount(), totalSteps);
        if (!changed) dvk::log::error("level-switch-test: the level list started no new level");
        if (!playing) dvk::log::error("level-switch-test: the new level is not being played");
        if (!inside) {
            dvk::log::error("level-switch-test: the ship is not inside the new level");
        }
        if (!stepped) dvk::log::error("level-switch-test: the new level ran no simulation steps");
        const bool ok = changed && playing && inside && stepped;
        dvk::log::info("level switch: {}", ok ? "PASS" : "FAIL");
        if (!ok) exitCode = 1;
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
    return exitCode;
}
