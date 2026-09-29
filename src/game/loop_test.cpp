#include "game/loop_test.h"

#include "core/log.h"
#include "game/bindings.h"
#include "game/level.h"
#include "game/level_list.h"
#include "game/menu.h"
#include "game/proxies.h"
#include "game/session.h"
#include "sim/collision.h"
#include "sim/combat.h"
#include "sim/gameflow.h"
#include "sim/robots.h"
#include "sim/weapons.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace dvk {

namespace {

constexpr float kStep = 1.0f / 120.0f;

struct Rig {
    Level level;
    sim::WeaponConfig weapons;
    sim::RobotConfig robotCfg;
    sim::GameConfig gameCfg;
    sim::RobotState robots;
    sim::CombatState combat;
    sim::GameState game;
    sim::Body player;
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};

    // Sound events raised over the whole test, by cue. This test is the only
    // thing that drives doors, keys, pickups, the reactor and the exit, so it
    // is the only place those cues can be shown to fire at all - a headless
    // run with nobody at the controls never reaches a door.
    sim::EventQueue events;
    std::array<int, static_cast<size_t>(sim::Cue::Count)> cues{};

    bool heard(sim::Cue cue) const { return cues[static_cast<size_t>(cue)] > 0; }

    void reset() {
        combat = sim::CombatState{};
        sim::initObjects(level, robotCfg, combat.objects);
        sim::spawnRobots(level, robotCfg, robots);
        sim::initGame(level, robots, game);
        player = sim::Body{};
        player.radius = 1.6f;
        player.position = level.start.position;
        player.segment = level.start.segment;
        combat.player.headlightOn = false;
    }

    // Robots are deliberately not stepped in most of these checks: this test is
    // about the level's flow, and a gunner emptying the player's shields
    // halfway through a door test would be noise.
    // The player's trigger, held across run() so a test can fire while it
    // steps.
    sim::FireControls held;

    void run(int steps, bool withRobots = false) {
        // Exactly the game's own order: doors first, so every geometric
        // question this step is asked against the same shut-door set.
        sim::SolidDoors doors;
        for (int i = 0; i < steps; ++i) {
            sim::buildSolidDoors(level, game, doors);
            events.clear();
            sim::stepCombat(level, combat, player, orientation, weapons, held, kStep, &robots,
                            &doors, &events);
            if (withRobots) {
                sim::stepRobots(level, robotCfg, weapons, robots, combat, player, kStep, &doors,
                                &events);
            }
            sim::stepGame(level, gameCfg, game, combat, robots, robotCfg, player, kStep, &events);
            for (int e = 0; e < events.count; ++e) {
                ++cues[static_cast<size_t>(events.items[static_cast<size_t>(e)].cue)];
            }
        }
    }

    void aimAt(const glm::vec3& target) {
        const glm::vec3 to = target - player.position;
        if (glm::length(to) < 1e-4f) return;
        orientation = glm::quatLookAt(glm::normalize(to), glm::vec3(0.0f, 1.0f, 0.0f));
    }

    int playerBolts() const {
        int n = 0;
        for (const sim::Projectile& p : combat.projectiles.items) {
            if (p.alive && p.owner < 0) ++n;
        }
        return n;
    }

    void teleport(const glm::vec3& position, int segment) {
        player.position = position;
        player.segment = segment >= 0 ? segment : findSegment(level, position);
        player.velocity = glm::vec3(0.0f);
    }
};

bool expect(bool condition, const char* what) {
    if (!condition) log::error("FAILED: {}", what);
    return condition;
}

bool verifyLevelSession(const Level& level, const sim::RobotConfig& robots) {
    bool ok = true;
    LevelSession session;
    ok &= expect(session.mode() == AppMode::MainMenu, "a session starts at the main menu");
    ok &= expect(!session.active(), "a session starts without an active level");

    session.load(level, robots);
    ok &= expect(session.mode() == AppMode::MainMenu, "loading keeps the session at the menu");
    session.start();
    const glm::vec3 initialPosition = session.current().ship.body.position;
    const int initialRobots = session.current().robots.count;
    const int initialObjects = session.current().combat.objects.count;
    ok &= expect(session.mode() == AppMode::Playing, "starting a level enters play");
    ok &= expect(session.advancesSimulation(), "play advances the simulation");

    session.current().game.elapsed = 12.0f;
    session.current().ship.body.position += glm::vec3(3.0f, 2.0f, 1.0f);
    session.pause();
    ok &= expect(session.mode() == AppMode::Paused, "pausing enters the paused state");
    ok &= expect(!session.advancesSimulation(), "pause freezes the simulation");
    session.resume();
    ok &= expect(session.mode() == AppMode::Playing, "resuming returns to play");

    session.finish();
    ok &= expect(session.mode() == AppMode::RunSummary, "finishing enters the run summary");
    ok &= expect(!session.advancesSimulation(), "the run summary freezes the simulation");
    session.restart();
    ok &= expect(session.mode() == AppMode::Playing, "restarting returns to play");
    ok &= expect(session.current().game.elapsed == 0.0f, "restarting resets elapsed time");
    ok &= expect(glm::distance(session.current().ship.body.position, initialPosition) < 1e-5f,
                 "restarting resets the player position");
    ok &= expect(session.current().robots.count == initialRobots, "restarting restores robots");
    ok &= expect(session.current().combat.objects.count == initialObjects,
                 "restarting restores objects");

    session.leave();
    ok &= expect(session.mode() == AppMode::MainMenu, "leaving returns to the main menu");
    ok &= expect(!session.active(), "leaving clears the active level");
    log::info("session lifecycle: {}", ok ? "PASS" : "FAIL");
    return ok;
}

bool verifyLevelList(const char* levelPath) {
    bool ok = true;
    const std::vector<LevelEntry> levels = discoverLevels("assets/levels", levelPath);
    ok &= expect(!levels.empty(), "the level directory lists at least one level");
    if (levels.empty()) {
        log::info("level list: FAIL");
        return false;
    }

    ok &= expect(levels.front().path == levelPath,
                 "the level a plain run would start is listed first");

    bool named = true;
    bool jsonOnly = true;
    for (const LevelEntry& entry : levels) {
        named &= !entry.name.empty();
        jsonOnly &= entry.path.size() > 5 && entry.path.compare(entry.path.size() - 5, 5, ".json") == 0;
        log::info("level '{}' at {}{}", entry.name, entry.path,
                  entry.comment.empty() ? "" : " (described)");
    }
    ok &= expect(named, "every listed level has a name");
    ok &= expect(jsonOnly, "only .json files are listed");

    // A missing directory is a menu with nothing in it, not a crash.
    ok &= expect(discoverLevels("assets/levels-that-are-not-there").empty(),
                 "a missing level directory lists nothing");
    log::info("level list: {}", ok ? "PASS" : "FAIL");
    return ok;
}

std::vector<LevelEntry> fakeLevels(int count) {
    std::vector<LevelEntry> levels;
    for (int i = 0; i < count; ++i) {
        LevelEntry entry;
        entry.path = std::format("assets/levels/mine{:02}.json", i);
        entry.name = std::format("mine{:02}", i);
        entry.comment = "A mine. It has rock in it, and the rock has robots in it.";
        levels.push_back(std::move(entry));
    }
    return levels;
}

bool verifyLevelSelect() {
    bool ok = true;
    constexpr int kWidth = 1600;
    constexpr int kHeight = 900;

    PlayerMenu menu;
    menu.setLevels(fakeLevels(3));
    menu.setSelectedLevel(1);
    menu.showLevelSelect();

    MenuView view = menu.view(kWidth, kHeight);
    ok &= expect(view.screen == MenuScreen::LevelSelect, "the level list is visible");
    ok &= expect(view.buttonCount == 4, "three levels and a back row");
    ok &= expect(view.buttons[1].selected && view.buttons[1].levelIndex == 1,
                 "the list opens on the level that is loaded");
    ok &= expect(std::string_view(view.subtitle).find("A mine.") == 0,
                 "the selected level's blurb is the subtitle");
    ok &= expect(std::string_view(view.subtitle).find("robots") == std::string_view::npos,
                 "only the first sentence of the blurb is kept");
    menu.setLevelRecord(menu.levels()[1].path, 2, 83);
    view = menu.view(kWidth, kHeight);
    ok &= expect(std::string_view(view.buttons[1].value.data()) == "best 1:23",
                 "completed levels show their best time");

    MenuInput confirm;
    confirm.confirm = true;
    ok &= expect(menu.update(confirm, kWidth, kHeight) == MenuAction::Start &&
                     menu.selectedLevel() == 1,
                 "confirming a row starts that level");

    // Down from the last level lands on back, and once more wraps to the top.
    MenuInput down;
    down.next = true;
    menu.update(down, kWidth, kHeight);   // -> level 2
    menu.update(down, kWidth, kHeight);   // -> back
    ok &= expect(menu.view(kWidth, kHeight).buttons[3].selected, "the cursor reaches back");
    ok &= expect(menu.update(confirm, kWidth, kHeight) == MenuAction::Back,
                 "back leaves the level list");
    menu.update(down, kWidth, kHeight);
    ok &= expect(menu.view(kWidth, kHeight).buttons[0].selected,
                 "the cursor wraps from back to the first level");

    // Clicking a row starts that level even when the cursor was elsewhere.
    view = menu.view(kWidth, kHeight);
    const MenuRect third = view.buttons[2].bounds;
    MenuInput click;
    click.pointer = {third.x + third.width * 0.5f, third.y + third.height * 0.5f};
    click.pointerPressed = true;
    ok &= expect(menu.update(click, kWidth, kHeight) == MenuAction::Start &&
                     menu.selectedLevel() == 2,
                 "clicking a row starts the level under the pointer");

    // A list longer than the screen shows a window that follows the cursor and
    // still fits inside the panel.
    PlayerMenu many;
    many.setLevels(fakeLevels(20));
    many.setSelectedLevel(19);
    many.showLevelSelect();
    view = many.view(kWidth, kHeight);
    ok &= expect(view.buttonCount == kMaxLevelRows + 3,
                 "a long list shows a window of rows, back, and a pair of scroll arrows");
    ok &= expect(view.levelsShown == kMaxLevelRows && view.levelCount == 20,
                 "the window reports which slice of the list it is");
    ok &= expect(view.firstLevelShown + view.levelsShown == 20,
                 "the window holds the selected level");
    bool onPanel = true;
    for (int i = 0; i < view.buttonCount; ++i) {
        const MenuRect& r = view.buttons[static_cast<size_t>(i)].bounds;
        onPanel &= r.y >= 0.0f && r.y + r.height <= static_cast<float>(kHeight) * 0.90f;
    }
    ok &= expect(onPanel, "every row of a full list stays inside the panel");

    // Small windows shrink the window rather than spilling rows off the panel.
    view = many.view(640, 360);
    bool onSmallPanel = true;
    for (int i = 0; i < view.buttonCount; ++i) {
        const MenuRect& r = view.buttons[static_cast<size_t>(i)].bounds;
        onSmallPanel &= r.y + r.height <= 360.0f * 0.90f + 0.01f;
    }
    ok &= expect(view.buttonCount >= 2 && onSmallPanel,
                 "a small window shows fewer rows rather than spilling them");

    // An empty directory is a list with nothing to pick, not a crash.
    PlayerMenu empty;
    empty.setLevels({});
    empty.showLevelSelect();
    view = empty.view(kWidth, kHeight);
    ok &= expect(view.buttonCount == 1 && view.buttons[0].action == MenuAction::Back,
                 "an empty level list offers only back");
    ok &= expect(empty.selectedLevel() == -1, "an empty level list selects nothing");
    ok &= expect(empty.update(confirm, kWidth, kHeight) == MenuAction::Back,
                 "confirming an empty level list backs out");

    log::info("level select: {}", ok ? "PASS" : "FAIL");
    return ok;
}

bool verifyPlayerMenu() {
    bool ok = true;
    PlayerMenu menu;
    menu.showMain();
    MenuView view = menu.view(1600, 900);
    ok &= expect(view.screen == MenuScreen::Main, "the main menu is visible");
    ok &= expect(view.buttonCount == 3, "the main menu has play, settings and quit");

    MenuInput keyboard;
    keyboard.next = true;
    menu.update(keyboard, 1600, 900);
    ok &= expect(menu.update(keyboard, 1600, 900) == MenuAction::None,
                 "keyboard navigation reaches the last menu action");
    MenuInput confirmLast;
    confirmLast.confirm = true;
    ok &= expect(menu.update(confirmLast, 1600, 900) == MenuAction::Quit,
                 "keyboard navigation selects a menu action");

    menu.showMain();
    MenuInput confirm;
    confirm.confirm = true;
    ok &= expect(menu.update(confirm, 1600, 900) == MenuAction::ChooseLevel,
                 "play opens the level list");

    menu.showPause();
    view = menu.view(1600, 900);
    const MenuRect resume = view.buttons[0].bounds;
    MenuInput pointer;
    pointer.pointer = {resume.x + resume.width * 0.5f, resume.y + resume.height * 0.5f};
    pointer.pointerPressed = true;
    ok &= expect(menu.update(pointer, 1600, 900) == MenuAction::Resume,
                 "pointer input activates a menu action");

    PlayerSettings settings;
    menu.showPause();
    menu.showSettings(settings);
    view = menu.view(1600, 900);
    ok &= expect(view.screen == MenuScreen::Settings && view.buttonCount == 5,
                 "settings open from the pause menu on the group list");
    ok &= expect(menu.update(confirm, 1600, 900) == MenuAction::OpenVideoSettings,
                 "the first settings group is video");

    menu.showSettingsScreen(MenuScreen::SettingsVideo);
    view = menu.view(1600, 900);
    ok &= expect(view.screen == MenuScreen::SettingsVideo && view.buttonCount == 7,
                 "video settings carry five values, apply and back");
    ok &= expect(std::string_view(view.buttons[1].value.data()) == "1600 x 900" &&
                     std::string_view(view.buttons[2].value.data()) == "on",
                 "video settings show their current values");
    ok &= expect(std::string_view(view.buttons[3].value.data()) == "70 deg" &&
                     std::string_view(view.buttons[4].value.data()) == "70 deg",
                 "field of view is a video setting, first and third person");

    // The tallest of the four, so it is the one the panel has to hold.
    view = menu.view(640, 360);
    bool settingsFit = true;
    for (int i = 0; i < view.buttonCount; ++i) {
        const MenuRect& bounds = view.buttons[static_cast<size_t>(i)].bounds;
        settingsFit &= bounds.y >= 360.0f * 0.12f && bounds.y + bounds.height <= 360.0f * 0.90f;
    }
    ok &= expect(settingsFit, "settings stay inside the panel at the smallest supported size");
    view = menu.view(2560, 1440);
    bool settingsClearHeader = true;
    for (int i = 0; i < view.buttonCount; ++i) {
        const MenuRect& bounds = view.buttons[static_cast<size_t>(i)].bounds;
        settingsClearHeader &= bounds.y >= 1440.0f * 0.30f &&
                               bounds.y + bounds.height <= 1440.0f * 0.88f;
    }
    ok &= expect(settingsClearHeader,
                 "settings stay below the header and inside the panel at 1440p");

    MenuInput nextSetting;
    nextSetting.next = true;
    MenuInput increase;
    increase.increase = true;
    for (int i = 0; i < 3; ++i) menu.update(nextSetting, 1600, 900);
    menu.update(increase, 1600, 900);
    ok &= expect(menu.editedSettings().firstPersonFov == 75.0f,
                 "field of view adjusts in five degree steps");

    menu.showSettingsScreen(MenuScreen::SettingsAudio);
    view = menu.view(1600, 900);
    ok &= expect(view.screen == MenuScreen::SettingsAudio && view.buttonCount == 5,
                 "audio settings carry three values, apply and back");
    ok &= expect(std::string_view(view.buttons[0].value.data()) == "80%" &&
                     std::string_view(view.buttons[1].value.data()) == "45%" &&
                     std::string_view(view.buttons[2].value.data()) == "100%",
                 "audio settings show their current values");
    MenuInput decrease;
    decrease.decrease = true;
    menu.update(decrease, 1600, 900);
    ok &= expect(menu.editedSettings().masterVolume == 0.75f,
                 "left adjusts the selected setting");

    menu.showSettingsScreen(MenuScreen::SettingsGame);
    view = menu.view(1600, 900);
    ok &= expect(view.screen == MenuScreen::SettingsGame && view.buttonCount == 6,
                 "game settings carry four values, apply and back");
    ok &= expect(std::string_view(view.buttons[0].value.data()) == "1.0x" &&
                     std::string_view(view.buttons[2].value.data()) == "on",
                 "game settings show mouse sensitivity and the tutorial toggle");
    for (int i = 0; i < 2; ++i) menu.update(nextSetting, 1600, 900);
    menu.update(increase, 1600, 900);
    ok &= expect(!menu.editedSettings().tutorials, "the tutorial toggle turns off");

    // Back out of each group in turn: the cursor lands on the row that opened
    // it rather than at the top, so walking the three of them does not start
    // over each time.
    menu.closeSettings();
    ok &= expect(menu.screen() == MenuScreen::Settings,
                 "closing a settings group returns to the group list");

    menu.showControls();
    view = menu.view(1600, 900);
    ok &= expect(view.screen == MenuScreen::Controls && view.buttonCount == 11 &&
                     view.controlCount == static_cast<int>(kPlayerActionCount),
                 "controls show a scrolling window of player actions");
    MenuInput beginCapture;
    beginCapture.confirm = true;
    menu.update(beginCapture, 1600, 900);
    ok &= expect(menu.capturingBinding(), "confirming an action waits for an input");
    MenuInput bind;
    bind.bindingPressed = true;
    bind.binding = {InputDevice::Keyboard, 83};
    menu.update(bind, 1600, 900);
    ok &= expect(menu.editedSettings().bindings[PlayerAction::Forward] == bind.binding &&
                     menu.editedSettings().bindings[PlayerAction::Back] ==
                         InputBinding{InputDevice::Keyboard, 87},
                 "rebinding swaps an existing conflict");
    // The weapon keys as a player finds them: read off the controls screen
    // rather than out of the defaults table, because the screen is where
    // anyone goes looking for which key fires what.
    {
        const auto rowValue = [&](PlayerAction wanted) {
            MenuInput step;
            step.next = true;
            // The list is a window over the actions, so walk the cursor to the
            // row instead of assuming it is on screen.
            for (size_t i = 0; i < kPlayerActionCount; ++i) {
                const MenuView rows = menu.view(1600, 900);
                for (int b = 0; b < rows.buttonCount; ++b) {
                    const MenuButtonView& row = rows.buttons[static_cast<size_t>(b)];
                    if (row.actionIndex == static_cast<int>(wanted) && row.selected) {
                        return std::string(row.value.data());
                    }
                }
                menu.update(step, 1600, 900);
            }
            return std::string("not shown");
        };
        const bool weaponKeys = rowValue(PlayerAction::SelectLaser) == "1" &&
                                rowValue(PlayerAction::SelectLightning) == "2" &&
                                rowValue(PlayerAction::SelectVulcan) == "3" &&
                                rowValue(PlayerAction::SelectRailgun) == "4" &&
                                rowValue(PlayerAction::CycleSecondary) == "G";
        ok &= expect(weaponKeys, "controls list the four primaries on 1-4 and the missiles on G");
    }

    view = menu.view(640, 360);
    bool controlsFit = true;
    for (int i = 0; i < view.buttonCount; ++i) {
        const MenuRect& bounds = view.buttons[static_cast<size_t>(i)].bounds;
        controlsFit &= bounds.y >= 360.0f * 0.12f && bounds.y + bounds.height <= 360.0f * 0.90f;
    }
    ok &= expect(controlsFit, "controls stay inside the panel at the smallest supported size");
    menu.closeControls();
    ok &= expect(menu.screen() == MenuScreen::Settings,
                 "closing controls returns to settings with edits intact");
    menu.closeSettings();
    ok &= expect(menu.screen() == MenuScreen::Pause,
                 "closing settings returns to the menu that opened them");

    menu.showDisplayConfirmation(15);
    view = menu.view(1600, 900);
    ok &= expect(view.screen == MenuScreen::DisplayConfirmation &&
                     view.confirmationSeconds == 15,
                 "display changes open a timed confirmation");

    menu.showSummary(true, "escaped", 42.9f, 12, 16, "new best time");
    view = menu.view(1600, 900);
    ok &= expect(view.screen == MenuScreen::Summary && view.won,
                 "a completed run opens the victory summary");
    ok &= expect(view.elapsedSeconds == 42 && view.robotsDestroyed == 12 &&
                     view.robotsTotal == 16,
                 "the run summary carries the result statistics");
    ok &= expect(std::string_view(view.recordNote) == "new best time",
                 "the run summary carries a record highlight");
    log::info("player menu: {}", ok ? "PASS" : "FAIL");
    return ok;
}

bool verifyInputBindings() {
    bool ok = true;
    InputBindings bindings = defaultInputBindings();
    assignInputBinding(bindings, PlayerAction::Forward,
                       {InputDevice::Keyboard, GLFW_KEY_I});

    Input input;
    input.beginFrame();
    input.onKey(GLFW_KEY_I, GLFW_PRESS);
    const sim::ShipControls controls = readControls(input, glm::vec2(0.0f), bindings);
    ok &= expect(controls.thrust == 1.0f &&
                     bindingPressed(input, bindings, PlayerAction::Forward),
                 "the configured keyboard binding drives its action");

    assignInputBinding(bindings, PlayerAction::PrimaryFire,
                       {InputDevice::Mouse, GLFW_MOUSE_BUTTON_4});
    input.beginFrame();
    input.onMouseButton(GLFW_MOUSE_BUTTON_4, GLFW_PRESS);
    ok &= expect(bindingDown(input, bindings, PlayerAction::PrimaryFire) &&
                     bindingPressed(input, bindings, PlayerAction::PrimaryFire),
                 "the configured mouse binding drives its action");

    char label[32]{};
    formatInputBinding(bindings[PlayerAction::PrimaryFire], label, sizeof(label));
    ok &= expect(std::string_view(label) == "mouse 4", "mouse bindings have player-facing names");
    log::info("input bindings: {}", ok ? "PASS" : "FAIL");
    return ok;
}

} // namespace

int runLoopTest(const char* levelPath, const char* weaponConfigPath, const char* robotConfigPath) {
    Rig rig;
    rig.level = loadLevel(levelPath);
    rig.weapons = sim::loadWeaponConfig(weaponConfigPath);
    rig.robotCfg = sim::loadRobotConfig(robotConfigPath);
    resolveRobotAttachments(rig.robotCfg);
    bool ok = true;
    ok &= verifyLevelSession(rig.level, rig.robotCfg);
    ok &= verifyPlayerMenu();
    ok &= verifyInputBindings();
    ok &= verifyLevelList(levelPath);
    ok &= verifyLevelSelect();

    // Only a campaign level has a loop to test. test01 is a renderer and
    // physics fixture with no doors, no exit and no reactor, and running the
    // rest of this against it indexed an empty vector and took the process
    // with it.
    if (rig.level.doors.empty() || !rig.level.exitTrigger.present) {
        log::info("'{}' is not a campaign level: {} doors, {} pickups, exit {}. Nothing to play "
                  "through - run this against mine01.",
                  rig.level.name, rig.level.doors.size(), rig.level.pickups.size(),
                  rig.level.exitTrigger.present ? "present" : "absent");
        log::info("loop test: SKIPPED");
        return 0;
    }

    ok &= expect(rig.level.doors.size() == 3, "the level has three keyed doors");
    ok &= expect(!rig.level.pickups.empty(), "the level has pickups");

    // --- each key opens its own door, and only its own ----------------------
    {
        for (size_t di = 0; di < rig.level.doors.size(); ++di) {
            const Door& door = rig.level.doors[di];
            const auto keyIndex = static_cast<size_t>(door.key);

            // Locked, standing right in front of it.
            rig.reset();
            rig.teleport(door.center - door.normal * 4.0f, door.segment);
            rig.run(240);
            const bool stayedShut = rig.game.doors[di].openness <= 0.01f;
            ok &= expect(stayedShut, "a locked door stayed shut without its key");

            // Every other key, and still nothing.
            rig.reset();
            for (size_t k = 0; k < rig.game.keys.size(); ++k) {
                rig.game.keys[k] = (k != keyIndex);
            }
            rig.teleport(door.center - door.normal * 4.0f, door.segment);
            rig.run(240);
            const bool wrongKeyFailed = rig.game.doors[di].openness <= 0.01f;
            ok &= expect(wrongKeyFailed, "the wrong keys did not open the door");

            // The right key, and it opens.
            rig.reset();
            rig.game.keys[keyIndex] = true;
            rig.teleport(door.center - door.normal * 4.0f, door.segment);
            rig.run(240);
            const bool opened = rig.game.doors[di].openness > 0.99f;
            ok &= expect(opened, "the right key opened the door");

            log::info("door {} ({}): locked {}, wrong key {}, right key {}", di,
                      kKeyNames[keyIndex], stayedShut ? "held" : "OPENED",
                      wrongKeyFailed ? "held" : "OPENED", opened ? "opened" : "STUCK");
        }
    }

    // --- a closed door blocks light -----------------------------------------
    //
    // The same DoorField the portal walk and collision use also stops the
    // light flood, so this is a check that all three agree.
    {
        rig.reset();
        const Door& door = rig.level.doors[0];

        Light probe;
        probe.position = rig.level.segments[static_cast<size_t>(door.segment)].center;
        probe.segment = door.segment;
        probe.radius = 90.0f;   // easily far enough to reach through
        LightFlood flood;

        rebuildLightSegments(rig.level, probe, flood, &rig.game.doorField);
        const bool blocked = std::find(probe.segments.begin(), probe.segments.end(),
                                       door.neighbour) == probe.segments.end();
        const size_t shutCount = probe.segments.size();

        rig.game.keys[static_cast<size_t>(door.key)] = true;
        rig.teleport(door.center - door.normal * 4.0f, door.segment);
        rig.run(240);
        rebuildLightSegments(rig.level, probe, flood, &rig.game.doorField);
        const bool reaches = std::find(probe.segments.begin(), probe.segments.end(),
                                       door.neighbour) != probe.segments.end();

        ok &= expect(blocked, "a shut door kept the light out of the next segment");
        ok &= expect(reaches, "the same light reached through once the door opened");
        log::info("light through a door: {} segments shut, {} open", shutCount,
                  probe.segments.size());

        // A door that has only just cracked open is already see-through -
        // sight and light go through the gap - while it still stops a body.
        // The portal walk and the flood consult the same `opaque` flag, so the
        // flood stands in for both here.
        //
        // The shot below is a GEOMETRIC test now that a door collides as its
        // two slabs rather than as one quad over the whole opening: the bolt
        // gets through because it is aimed at the parting between the halves,
        // and an identical bolt aimed a little to either side would not. So the
        // openness here has to leave a gap wider than the bolt while staying
        // under kPassableAt, or the case stops measuring what it says it does -
        // at a fifteenth open, door 0 of mine01 parts by about three units
        // against a laser bolt seven tenths across.
        rig.reset();
        rig.game.doors[0].openness = 0.15f;
        rig.run(1);
        const bool seeThrough = !rig.game.doorField.isOpaque(door.segment, door.side);
        const bool stillBlocks = rig.game.doorField.isBlocked(door.segment, door.side);
        rebuildLightSegments(rig.level, probe, flood, &rig.game.doorField);
        const bool litThroughGap = std::find(probe.segments.begin(), probe.segments.end(),
                                             door.neighbour) != probe.segments.end();
        sim::SolidDoors crackedDoors;
        sim::buildSolidDoors(rig.level, rig.game, crackedDoors);
        const glm::vec3 shotFrom = door.center + door.normal * 8.0f;
        const sim::RayHit crackedShot = sim::sweepSphere(
            rig.level, shotFrom, -door.normal,
            rig.weapons.weapons[static_cast<size_t>(sim::WeaponLaser)].radius, 9.0f,
            door.segment, &crackedDoors);
        const bool shotThroughGap = !crackedShot.hit;
        ok &= expect(seeThrough, "a door that has started opening is see-through");
        ok &= expect(stillBlocks, "a door that has started opening still blocks a body");
        ok &= expect(litThroughGap, "light reaches through a door that has started opening");
        ok &= expect(shotThroughGap,
                     "a projectile passes through a door that has started opening");
        log::info("cracked door at {:.2f}: see-through {}, passable {}, lit through {}, shot "
                  "through {}",
                  rig.game.doors[0].openness, seeThrough ? "yes" : "NO",
                  stillBlocks ? "no" : "YES", litThroughGap ? "yes" : "NO",
                  shotThroughGap ? "yes" : "NO");

        // And once the level's own lights are rebuilt against the field, a
        // level light behind a shut door stays out of the room in front.
        rig.reset();
        LightFlood levelFlood;
        rebuildAllLightSegments(rig.level, levelFlood, &rig.game.doorField);
        int crossings = 0;
        for (const Light& l : rig.level.lights) {
            const bool inFront = std::find(l.segments.begin(), l.segments.end(),
                                           door.segment) != l.segments.end();
            const bool inBack = std::find(l.segments.begin(), l.segments.end(),
                                          door.neighbour) != l.segments.end();
            // Both sides at once is only possible through the shut door or
            // around a loop; mine01's doors have no loop around them.
            if (inFront && inBack) ++crossings;
        }
        ok &= expect(crossings == 0, "no level light reaches both sides of a shut door");
        // For scale: the same count with no door state at all, which is what
        // the level lights carried from load until now.
        int blindCrossings = 0;
        rebuildAllLightSegments(rig.level, levelFlood, nullptr);
        for (const Light& l : rig.level.lights) {
            const bool inFront = std::find(l.segments.begin(), l.segments.end(),
                                           door.segment) != l.segments.end();
            const bool inBack = std::find(l.segments.begin(), l.segments.end(),
                                          door.neighbour) != l.segments.end();
            if (inFront && inBack) ++blindCrossings;
        }
        log::info("level lights vs shut door 0: {} of {} reach both sides with doors, {} "
                  "without",
                  crossings, rig.level.lights.size(), blindCrossings);
    }

    // --- a shut door stops bolts, sight lines and robots ----------------------
    //
    // The door faces are built from the same threshold as the DoorField and
    // handed to combat and robots alike. Before this only the player's own
    // collision knew about doors: robots saw, shot and flew straight through.
    {
        const Door& door = rig.level.doors[0];
        const auto keyIndex = static_cast<size_t>(door.key);
        // Side normals point into their own segment, so this is inside the
        // door's segment, a few units back from the slab.
        const glm::vec3 inFront = door.center + door.normal * 8.0f;
        const glm::vec3 behind = rig.level.segments[static_cast<size_t>(door.neighbour)].center;

        // A bolt fired at a shut door stops at it.
        rig.reset();
        rig.combat.player.headlightOn = false;
        rig.teleport(inFront, door.segment);
        rig.aimAt(door.center);
        rig.held = sim::FireControls{};
        rig.held.selectWeapon = sim::WeaponLaser;
        rig.held.primary = true;
        rig.run(1);
        rig.held = sim::FireControls{};
        bool crossed = false;
        float furthest = 0.0f;
        for (int i = 0; i < 240 && rig.playerBolts() > 0; ++i) {
            rig.run(1);
            for (const sim::Projectile& p : rig.combat.projectiles.items) {
                if (!p.alive || p.owner >= 0) continue;
                // Signed distance past the door plane, positive beyond it.
                const float past = -glm::dot(p.position - door.center, door.normal);
                furthest = std::max(furthest, past);
                if (past > 1.0f) crossed = true;
            }
        }
        ok &= expect(!crossed, "a bolt did not pass through a shut door");
        ok &= expect(rig.playerBolts() == 0, "the bolt hit the door and expired");

        // With the door open the same shot carries on into the next segment.
        rig.reset();
        rig.combat.player.headlightOn = false;
        rig.game.keys[keyIndex] = true;
        rig.teleport(inFront, door.segment);
        rig.run(240);
        ok &= expect(rig.game.doors[0].openness > 0.99f, "the door opened for the key");
        rig.aimAt(door.center);
        rig.held.selectWeapon = sim::WeaponLaser;
        rig.held.primary = true;
        rig.run(1);
        rig.held = sim::FireControls{};
        bool carriedOn = false;
        for (int i = 0; i < 240 && rig.playerBolts() > 0; ++i) {
            rig.run(1);
            for (const sim::Projectile& p : rig.combat.projectiles.items) {
                if (!p.alive || p.owner >= 0) continue;
                if (-glm::dot(p.position - door.center, door.normal) > 1.0f) carriedOn = true;
            }
        }
        ok &= expect(carriedOn, "the same bolt flew through once the door was open");
        log::info("door vs bolts: shut stopped it {:.1f} units short of the plane, open let it "
                  "through",
                  -furthest);

        // A robot behind the shut door neither sees nor shoots the player,
        // and cannot get through. Every other robot is removed so nothing
        // else in the level can shoot the player during the check.
        rig.reset();
        rig.combat.player.headlightOn = false;
        sim::Robot* gunner = nullptr;
        for (int i = 0; i < rig.robots.count; ++i) {
            sim::Robot& r = rig.robots.robots[static_cast<size_t>(i)];
            if (!gunner && r.alive && rig.robotCfg.types[static_cast<size_t>(r.type)].name == "gunner") {
                gunner = &r;
            } else {
                r.alive = false;
            }
        }
        ok &= expect(gunner != nullptr, "the level has a gunner");
        if (gunner) {
            gunner->body.position = behind;
            gunner->body.segment = door.neighbour;
            rig.teleport(inFront, door.segment);
            rig.aimAt(door.center);
            const float shields = rig.combat.player.shields;

            int throughDoor = 0;
            for (int i = 0; i < 600; ++i) {
                // Kept hunting, so this tests the door and not perception.
                gunner->awareness = 1.0f;
                gunner->hasLastKnown = true;
                gunner->lastKnown = rig.player.position;
                rig.run(1, true);
                if (gunner->body.segment == door.segment) ++throughDoor;
            }
            const float shotThrough = shields - rig.combat.player.shields;
            ok &= expect(shotThrough <= 0.0f, "no robot fire came through the shut door");
            ok &= expect(throughDoor == 0, "the robot did not get through the locked door");
            ok &= expect(rig.game.doors[0].openness <= 0.01f,
                         "the locked door did not open for the robot");
            const float gap = glm::dot(gunner->body.position - door.center, door.normal);
            log::info("door vs robot: 5 s of chasing, {} steps through, door {:.2f} open, robot "
                      "{:.1f} units on its own side, shields lost {:.1f}",
                      throughDoor, rig.game.doors[0].openness, -gap, shotThrough);

            // Line of sight: with the door shut a fresh robot never notices
            // the player; with it open it does.
            gunner->awareness = 0.0f;
            gunner->hasLastKnown = false;
            gunner->mode = sim::RobotMode::Idle;
            gunner->body.position = behind;
            gunner->body.velocity = glm::vec3(0.0f);
            gunner->body.segment = door.neighbour;
            rig.run(120, true);
            const bool sawThroughShut = gunner->awareness > 0.0f;
            ok &= expect(!sawThroughShut, "a robot cannot see through a shut door");

            rig.game.keys[keyIndex] = true;
            rig.run(360, true);
            const bool sawThroughOpen = gunner->awareness > 0.0f;
            ok &= expect(sawThroughOpen, "the same robot sees the player once the door opens");
            log::info("door vs sight: shut {}, open {}", sawThroughShut ? "SEEN" : "unseen",
                      sawThroughOpen ? "seen" : "UNSEEN");
        }
    }

    // --- the reactor starts the countdown ------------------------------------
    {
        rig.reset();
        int reactor = -1;
        for (int i = 0; i < rig.robots.count; ++i) {
            const sim::Robot& r = rig.robots.robots[static_cast<size_t>(i)];
            if (rig.robotCfg.types[static_cast<size_t>(r.type)].isReactor) reactor = i;
        }
        ok &= expect(reactor >= 0, "the level has a reactor");
        ok &= expect(rig.game.phase == sim::GamePhase::Playing, "the level starts in play");

        if (reactor >= 0) {
            const float health = rig.robots.robots[static_cast<size_t>(reactor)].health;
            rig.robots.robots[static_cast<size_t>(reactor)].health = -1.0f;
            // Robots are stepped here, because it is stepRobots that notices a
            // robot is dead and kills it.
            rig.run(4, true);

            ok &= expect(rig.game.phase == sim::GamePhase::Countdown,
                         "destroying the reactor started the countdown");
            ok &= expect(rig.game.exitOpen, "the exit unlocked");
            ok &= expect(std::abs(rig.game.countdown - rig.gameCfg.countdownSeconds) < 0.5f,
                         "the countdown started at its configured length");
            log::info("reactor: {:.0f} health, destroyed -> countdown {:.1f}s, exit open",
                      health, rig.game.countdown);

            // The shake ramps up as the clock runs down.
            const float shakeAtStart = rig.game.shake;
            rig.run(static_cast<int>(30.0f / kStep));
            const float shakeLater = rig.game.shake;
            ok &= expect(shakeLater > shakeAtStart, "the mine shakes harder as the clock runs out");
            log::info("shake: {:.3f} at the start of the countdown, {:.3f} thirty seconds in",
                      shakeAtStart, shakeLater);
        }
    }

    // --- reaching the exit wins -----------------------------------------------
    {
        rig.reset();
        for (int i = 0; i < rig.robots.count; ++i) {
            const sim::Robot& r = rig.robots.robots[static_cast<size_t>(i)];
            if (rig.robotCfg.types[static_cast<size_t>(r.type)].isReactor) {
                rig.robots.robots[static_cast<size_t>(i)].health = -1.0f;
            }
        }
        rig.run(4, true);
        ok &= expect(rig.game.phase == sim::GamePhase::Countdown, "countdown running");

        rig.teleport(rig.level.exitTrigger.position, rig.level.exitTrigger.segment);
        rig.run(4);
        ok &= expect(rig.game.phase == sim::GamePhase::Won, "reaching the exit won the level");
        log::info("escape: won with {:.1f}s left, {} robots destroyed", rig.game.countdown,
                  rig.game.robotsDestroyed);
    }

    // --- letting the clock run out loses ---------------------------------------
    {
        rig.reset();
        for (int i = 0; i < rig.robots.count; ++i) {
            const sim::Robot& r = rig.robots.robots[static_cast<size_t>(i)];
            if (rig.robotCfg.types[static_cast<size_t>(r.type)].isReactor) {
                rig.robots.robots[static_cast<size_t>(i)].health = -1.0f;
            }
        }
        rig.run(4, true);
        rig.run(static_cast<int>((rig.gameCfg.countdownSeconds + 1.0f) / kStep));
        ok &= expect(rig.game.phase == sim::GamePhase::Lost, "the countdown running out lost");
        log::info("timeout: lost, reason '{}'", rig.game.endReason);
    }

    // --- shields to zero loses --------------------------------------------------
    {
        rig.reset();
        rig.combat.player.shields = 0.0f;
        rig.run(2);
        ok &= expect(rig.game.phase == sim::GamePhase::Lost, "zero shields lost");
        log::info("death: lost, reason '{}'", rig.game.endReason);
    }

    // --- every kind of pickup does something -------------------------------------
    {
        int kinds = 0;
        for (size_t pi = 0; pi < rig.level.pickups.size(); ++pi) {
            rig.reset();
            const Pickup& pickup = rig.level.pickups[pi];

            // Spend something first, so a pickup that tops up has room to.
            rig.combat.player.shields = 40.0f;
            rig.combat.player.energy = 40.0f;
            rig.combat.player.lightningAmmo = 0;
            const sim::PlayerState before = rig.combat.player;
            const auto keysBefore = rig.game.keys;

            rig.teleport(pickup.position, pickup.segment);
            rig.run(4);

            const sim::PlayerState& after = rig.combat.player;
            const bool changed =
                after.shields != before.shields || after.energy != before.energy ||
                after.laserLevel != before.laserLevel ||
                after.concussionAmmo != before.concussionAmmo ||
                after.homingAmmo != before.homingAmmo ||
                after.railgunAmmo != before.railgunAmmo ||
                after.lightningAmmo != before.lightningAmmo ||
                after.flares != before.flares ||
                rig.game.keys != keysBefore;

            ok &= expect(rig.game.collected[pi], "the pickup was collected on touch");
            ok &= expect(changed, "the pickup changed something");
            if (changed) ++kinds;
        }
        log::info("pickups: {} of {} collected and had an effect", kinds,
                  rig.level.pickups.size());
    }

    // --- what the automap is allowed to know ---------------------------------
    //
    // The layout itself is drawn from the start, but everything in it is still
    // earned, and `visited` still has to be an honest record of where the ship
    // went - it is what shades the rooms already flown. So each of these checks
    // the negative before the positive: nothing is marked until the thing that
    // marks it happens.
    // The sighting query itself is handed the shut-door set, so it inherits the
    // door blocking the loop test already proves for robot line of sight above.
    {
        rig.reset();
        rig.run(2);

        const int visitedAtStart = rig.game.visitedCount;
        ok &= expect(visitedAtStart == 1, "only the starting segment is visited at the start");
        ok &= expect(rig.game.visited[static_cast<size_t>(rig.level.start.segment)],
                     "the starting segment is the one marked visited");

        int unvisited = 0;
        for (int si = 0; si < rig.level.segmentCount(); ++si) {
            if (!rig.game.visited[static_cast<size_t>(si)]) ++unvisited;
        }
        ok &= expect(unvisited == rig.level.segmentCount() - 1,
                     "every other segment is still unflown");

        // Walking the graph marks exactly the rooms walked through.
        int walked = 0;
        for (int si = 0; si < rig.level.segmentCount() && walked < 6; ++si) {
            rig.teleport(rig.level.segments[static_cast<size_t>(si)].center, si);
            rig.run(2);
            ++walked;
        }
        ok &= expect(rig.game.visitedCount == walked,
                     "visiting N segments marks exactly N of them");
        log::info("automap: {} of {} segments visited after walking {}", rig.game.visitedCount,
                  rig.level.segmentCount(), walked);
    }

    // A key is on the map once the player has been in the room with it, and not
    // before.
    {
        rig.reset();
        int keyIndex = -1;
        for (size_t pi = 0; pi < rig.level.pickups.size(); ++pi) {
            if (rig.level.pickups[pi].kind >= PickupKind::KeyBlue) {
                keyIndex = static_cast<int>(pi);
                break;
            }
        }
        if (keyIndex >= 0) {
            const size_t ki = static_cast<size_t>(keyIndex);
            rig.run(2);
            ok &= expect(!rig.game.seen[ki], "a key in an unvisited room is not on the map");

            // Into the room, but stopped short of the key itself.
            const Pickup& key = rig.level.pickups[ki];
            rig.teleport(rig.level.segments[static_cast<size_t>(key.segment)].center, key.segment);
            rig.run(2);
            ok &= expect(rig.game.seen[ki], "entering the room puts the key on the map");
            log::info("automap: key {} seen={} collected={}", keyIndex, rig.game.seen[ki],
                      rig.game.collected[ki]);
        } else {
            log::info("automap: no keys on this level, key visibility not checked");
        }
    }

    // The reactor is marked only once it has actually been looked at.
    {
        rig.reset();
        int reactorIndex = -1;
        for (int i = 0; i < rig.robots.count; ++i) {
            const sim::Robot& r = rig.robots.robots[static_cast<size_t>(i)];
            if (rig.robotCfg.types[static_cast<size_t>(r.type)].isReactor) reactorIndex = i;
        }
        if (reactorIndex >= 0) {
            const sim::Robot& reactor = rig.robots.robots[static_cast<size_t>(reactorIndex)];
            rig.run(2);
            ok &= expect(!rig.game.reactorSeen, "the reactor is not on the map from the start");

            // Standing in its room is line of sight to it.
            const glm::vec3 reactorAt = reactor.body.position;
            rig.teleport(reactorAt + glm::vec3(0.0f, 0.0f, 6.0f), reactor.body.segment);
            rig.run(2);
            ok &= expect(rig.game.reactorSeen, "standing in front of the reactor sights it");
            ok &= expect(glm::length(rig.game.reactorPosition - reactorAt) < 1.0f,
                         "the sighted position is the reactor's");
            log::info("automap: reactor sighted at {:.1f} {:.1f} {:.1f}",
                      rig.game.reactorPosition.x, rig.game.reactorPosition.y,
                      rig.game.reactorPosition.z);
        } else {
            log::info("automap: no reactor on this level, sighting not checked");
        }
    }

    // --- sound ------------------------------------------------------------
    //
    // Not a check of how anything sounds - that is --sound-test's job - but of
    // whether the simulation raises the cue at all. A cue nothing emits and a
    // cue whose situation never arose look identical from outside, and this is
    // the only test that reaches a door, a key, the reactor and the exit.
    {
        std::string fired;
        int distinct = 0;
        for (size_t i = 0; i < rig.cues.size(); ++i) {
            if (rig.cues[i] == 0) continue;
            ++distinct;
            if (!fired.empty()) fired += ", ";
            fired += std::format("{} x{}", sim::kCueNames[i], rig.cues[i]);
        }
        log::info("sound cues raised: {} of {}", distinct,
                  static_cast<int>(rig.cues.size()));
        log::info("  {}", fired.empty() ? "none" : fired);

        // The cues this test is the only thing that can reach. Each one is a
        // situation the test above has already asserted actually happened, so a
        // silent one here means nothing emits it, not that the level is odd.
        static constexpr std::array<sim::Cue, 6> kFlowCues{
            {sim::Cue::DoorOpen, sim::Cue::DoorLocked, sim::Cue::Pickup, sim::Cue::KeyPickup,
             sim::Cue::ReactorDestroyed, sim::Cue::Exit}};
        for (sim::Cue cue : kFlowCues) {
            if (!rig.heard(cue)) {
                log::error("FAILED: nothing raised the {} cue",
                           sim::kCueNames[static_cast<size_t>(cue)]);
                ok = false;
            }
        }
        if (rig.events.dropped > 0) {
            log::error("FAILED: {} sound event(s) dropped - the queue is too small",
                       rig.events.dropped);
            ok = false;
        }
    }

    log::info("loop test: {}", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace dvk
