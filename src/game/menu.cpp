#include "game/menu.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <utility>

namespace dvk {

namespace {

struct ButtonDefinition {
    MenuAction action;
    const char* label;
};

constexpr std::array<ButtonDefinition, 3> kMainButtons{{
    {MenuAction::ChooseLevel, "play"},
    {MenuAction::OpenSettings, "settings"},
    {MenuAction::Quit, "quit"},
}};

constexpr std::array<ButtonDefinition, 5> kPauseButtons{{
    {MenuAction::Resume, "resume"},
    {MenuAction::OpenSettings, "settings"},
    {MenuAction::Restart, "restart level"},
    {MenuAction::MainMenu, "main menu"},
    {MenuAction::Quit, "quit"},
}};

constexpr std::array<ButtonDefinition, 2> kSummaryButtons{{
    {MenuAction::Restart, "retry"},
    {MenuAction::MainMenu, "main menu"},
}};

constexpr std::array<ButtonDefinition, 11> kSettingsButtons{{
    {MenuAction::None, "master volume"},
    {MenuAction::None, "mouse sensitivity"},
    {MenuAction::None, "1st person fov"},
    {MenuAction::None, "3rd person fov"},
    {MenuAction::None, "invert mouse y"},
    {MenuAction::None, "fullscreen"},
    {MenuAction::None, "window size"},
    {MenuAction::None, "vsync"},
    {MenuAction::OpenControls, "controls"},
    {MenuAction::ApplySettings, "apply"},
    {MenuAction::Back, "cancel"},
}};

constexpr std::array<ButtonDefinition, 2> kDisplayConfirmationButtons{{
    {MenuAction::KeepDisplay, "keep changes"},
    {MenuAction::RevertDisplay, "revert"},
}};

constexpr std::array<std::pair<int, int>, 4> kWindowSizes{{
    {1280, 720},
    {1600, 900},
    {1920, 1080},
    {2560, 1440},
}};

std::pair<const ButtonDefinition*, int> buttonsFor(MenuScreen screen) {
    switch (screen) {
        case MenuScreen::Main: return {kMainButtons.data(), static_cast<int>(kMainButtons.size())};
        case MenuScreen::Pause:
            return {kPauseButtons.data(), static_cast<int>(kPauseButtons.size())};
        case MenuScreen::Settings:
            return {kSettingsButtons.data(), static_cast<int>(kSettingsButtons.size())};
        case MenuScreen::Controls:
            return {nullptr, static_cast<int>(kPlayerActionCount) + 2};
        case MenuScreen::DisplayConfirmation:
            return {kDisplayConfirmationButtons.data(),
                    static_cast<int>(kDisplayConfirmationButtons.size())};
        case MenuScreen::Summary:
            return {kSummaryButtons.data(), static_cast<int>(kSummaryButtons.size())};
        case MenuScreen::LevelSelect:   // built from the level list, not a table
        case MenuScreen::Hidden: return {nullptr, 0};
    }
    return {nullptr, 0};
}

}

void PlayerMenu::setLevels(std::vector<LevelEntry> levels) {
    levels_ = std::move(levels);
    // A level's comment is written for whoever opens the file - mine01's is a
    // paragraph about where the keys are. The list wants a blurb, so keep the
    // first sentence and drop the rest. The renderer still cuts what does not
    // fit; this is about what is worth showing, not about what fits.
    for (LevelEntry& entry : levels_) {
        const size_t stop = entry.comment.find(". ");
        if (stop != std::string::npos) entry.comment.resize(stop + 1);
    }
    const int count = static_cast<int>(levels_.size());
    levelSelection_ = std::clamp(levelSelection_, 0, count);
    chosenLevel_ = count == 0 ? -1 : std::clamp(chosenLevel_, 0, count - 1);
}

void PlayerMenu::setSelectedLevel(int index) {
    if (levels_.empty()) {
        levelSelection_ = 0;
        chosenLevel_ = -1;
        return;
    }
    chosenLevel_ = std::clamp(index, 0, static_cast<int>(levels_.size()) - 1);
    levelSelection_ = chosenLevel_;
}

void PlayerMenu::setLevelRecord(const std::string& path, int completions, int bestSeconds) {
    const auto found = std::find_if(levels_.begin(), levels_.end(),
                                    [&](const LevelEntry& level) { return level.path == path; });
    if (found == levels_.end()) return;
    found->completions = std::max(completions, 0);
    found->bestSeconds = bestSeconds;
}

void PlayerMenu::hide() {
    screen_ = MenuScreen::Hidden;
    selection_ = 0;
}

void PlayerMenu::showMain() {
    screen_ = MenuScreen::Main;
    selection_ = 0;
}

void PlayerMenu::showLevelSelect() {
    screen_ = MenuScreen::LevelSelect;
    // Not reset: the cursor stays on the level that was last picked, so
    // backing out of a run and coming here again does not start over at the
    // top of the list.
    levelSelection_ = std::clamp(levelSelection_, 0, static_cast<int>(levels_.size()));
}

void PlayerMenu::showPause() {
    screen_ = MenuScreen::Pause;
    selection_ = 0;
}

void PlayerMenu::showSettings(const PlayerSettings& settings) {
    settingsParent_ = screen_ == MenuScreen::Pause ? MenuScreen::Pause : MenuScreen::Main;
    editedSettings_ = normalizedPlayerSettings(settings);
    screen_ = MenuScreen::Settings;
    selection_ = 0;
}

void PlayerMenu::showDisplayConfirmation(int seconds) {
    screen_ = MenuScreen::DisplayConfirmation;
    selection_ = 0;
    confirmationSeconds_ = std::max(seconds, 0);
}

void PlayerMenu::showControls() {
    screen_ = MenuScreen::Controls;
    controlSelection_ = std::clamp(controlSelection_, 0, static_cast<int>(kPlayerActionCount) + 1);
    capturingBinding_ = false;
}

void PlayerMenu::closeControls() {
    screen_ = MenuScreen::Settings;
    selection_ = 6;
    capturingBinding_ = false;
}

void PlayerMenu::closeSettings() {
    screen_ = settingsParent_;
    selection_ = 0;
}

void PlayerMenu::setConfirmationSeconds(int seconds) {
    confirmationSeconds_ = std::max(seconds, 0);
}

void PlayerMenu::showSummary(bool won, const char* reason, float elapsed, int robotsDestroyed,
                             int robotsTotal, const char* recordNote) {
    screen_ = MenuScreen::Summary;
    selection_ = 0;
    won_ = won;
    reason_ = reason;
    recordNote_ = recordNote;
    elapsedSeconds_ = std::max(0, static_cast<int>(elapsed));
    robotsDestroyed_ = robotsDestroyed;
    robotsTotal_ = robotsTotal;
}

// How many things the cursor can sit on. For the level list that is one per
// level plus "back", which is more than the rows on screen when the list is
// long: the cursor moves through the whole list and the window follows it.
int PlayerMenu::buttonCount() const {
    if (screen_ == MenuScreen::LevelSelect) return static_cast<int>(levels_.size()) + 1;
    if (screen_ == MenuScreen::Controls) return static_cast<int>(kPlayerActionCount) + 2;
    return buttonsFor(screen_).second;
}

void PlayerMenu::adjustSetting(int index, int direction) {
    if (direction == 0) return;
    switch (index) {
        case 0:
            editedSettings_.masterVolume =
                std::clamp(editedSettings_.masterVolume + static_cast<float>(direction) * 0.05f,
                           0.0f, 1.0f);
            break;
        case 1:
            editedSettings_.mouseSensitivity =
                std::clamp(editedSettings_.mouseSensitivity + static_cast<float>(direction) * 0.1f,
                           0.2f, 3.0f);
            break;
        case 2:
            editedSettings_.firstPersonFov =
                std::clamp(editedSettings_.firstPersonFov + static_cast<float>(direction) * 5.0f,
                           kMinVerticalFov, kMaxVerticalFov);
            break;
        case 3:
            editedSettings_.thirdPersonFov =
                std::clamp(editedSettings_.thirdPersonFov + static_cast<float>(direction) * 5.0f,
                           kMinVerticalFov, kMaxVerticalFov);
            break;
        case 4: editedSettings_.invertMouseY = !editedSettings_.invertMouseY; break;
        case 5: editedSettings_.fullscreen = !editedSettings_.fullscreen; break;
        case 6: {
            int closest = 0;
            int closestDistance = 1 << 30;
            for (int i = 0; i < static_cast<int>(kWindowSizes.size()); ++i) {
                const auto [width, height] = kWindowSizes[static_cast<size_t>(i)];
                const int distance = std::abs(width - editedSettings_.windowWidth) +
                                     std::abs(height - editedSettings_.windowHeight);
                if (distance < closestDistance) {
                    closest = i;
                    closestDistance = distance;
                }
            }
            const int count = static_cast<int>(kWindowSizes.size());
            closest = (closest + direction + count) % count;
            editedSettings_.windowWidth = kWindowSizes[static_cast<size_t>(closest)].first;
            editedSettings_.windowHeight = kWindowSizes[static_cast<size_t>(closest)].second;
            break;
        }
        case 7: editedSettings_.vsync = !editedSettings_.vsync; break;
        default: break;
    }
}

MenuAction PlayerMenu::update(const MenuInput& input, int width, int height) {
    const int count = buttonCount();
    if (count == 0) return MenuAction::None;

    if (screen_ == MenuScreen::Controls && capturingBinding_) {
        if (input.cancel) {
            capturingBinding_ = false;
        } else if (input.bindingPressed) {
            assignInputBinding(editedSettings_.bindings,
                               static_cast<PlayerAction>(controlSelection_), input.binding);
            capturingBinding_ = false;
        }
        return MenuAction::None;
    }

    const bool levelList = screen_ == MenuScreen::LevelSelect;
    const bool controlList = screen_ == MenuScreen::Controls;
    int& cursor = levelList ? levelSelection_ : (controlList ? controlSelection_ : selection_);

    const MenuView current = view(width, height);
    for (int i = 0; i < current.buttonCount; ++i) {
        const MenuButtonView& button = current.buttons[static_cast<size_t>(i)];
        if (!button.bounds.contains(input.pointer)) continue;
        // On the level list a button is not its own index: the rows show a
        // window into the list, and "back" sits past the end of it.
        if (levelList) cursor = button.levelIndex >= 0 ? button.levelIndex : count - 1;
        else if (controlList) cursor = button.actionIndex;
        else cursor = i;
    }

    if (input.previous) cursor = (cursor + count - 1) % count;
    if (input.next) cursor = (cursor + 1) % count;

    if (screen_ == MenuScreen::Settings && selection_ < 8) {
        if (input.decrease) adjustSetting(selection_, -1);
        if (input.increase) adjustSetting(selection_, 1);
    }

    const MenuView updated = view(width, height);
    if (input.pointerPressed) {
        for (int i = 0; i < updated.buttonCount; ++i) {
            const MenuButtonView& button = updated.buttons[static_cast<size_t>(i)];
            if (!button.bounds.contains(input.pointer)) continue;
            if (button.levelIndex >= 0) chosenLevel_ = button.levelIndex;
            if (controlList) {
                if (button.actionIndex < static_cast<int>(kPlayerActionCount)) {
                    capturingBinding_ = true;
                } else if (button.actionIndex == static_cast<int>(kPlayerActionCount)) {
                    editedSettings_.bindings = defaultInputBindings();
                } else {
                    return MenuAction::Back;
                }
                return MenuAction::None;
            }
            if (screen_ == MenuScreen::Settings && i < 8) {
                adjustSetting(i, 1);
                return MenuAction::None;
            }
            return button.action;
        }
    }
    if (input.confirm) {
        if (levelList) {
            if (levelSelection_ >= static_cast<int>(levels_.size())) return MenuAction::Back;
            chosenLevel_ = levelSelection_;
            return MenuAction::Start;
        }
        if (controlList) {
            if (controlSelection_ < static_cast<int>(kPlayerActionCount)) {
                capturingBinding_ = true;
            } else if (controlSelection_ == static_cast<int>(kPlayerActionCount)) {
                editedSettings_.bindings = defaultInputBindings();
            } else {
                return MenuAction::Back;
            }
            return MenuAction::None;
        }
        if (screen_ == MenuScreen::Settings && selection_ < 8) {
            adjustSetting(selection_, 1);
            return MenuAction::None;
        }
        const auto [definitions, definitionCount] = buttonsFor(screen_);
        if (selection_ < definitionCount) return definitions[selection_].action;
    }
    return MenuAction::None;
}

MenuView PlayerMenu::view(int width, int height) const {
    MenuView result;
    result.screen = screen_;
    result.won = won_;
    result.reason = reason_;
    result.recordNote = recordNote_;
    result.elapsedSeconds = elapsedSeconds_;
    result.robotsDestroyed = robotsDestroyed_;
    result.robotsTotal = robotsTotal_;
    result.confirmationSeconds = confirmationSeconds_;

    const float screenWidth = static_cast<float>(std::max(width, 1));
    const float screenHeight = static_cast<float>(std::max(height, 1));
    const float buttonWidth = std::clamp(screenWidth * 0.32f, 260.0f, 460.0f);
    const float left = (screenWidth - buttonWidth) * 0.5f;
    const float gap = std::clamp(screenHeight * 0.018f, 10.0f, 18.0f);

    if (screen_ == MenuScreen::LevelSelect) {
        const int levelCount = static_cast<int>(levels_.size());
        result.title = "select level";
        result.levelCount = levelCount;
        if (levelCount == 0) {
            result.subtitle = "no levels in assets/levels";
        } else if (levelSelection_ < levelCount) {
            const LevelEntry& entry = levels_[static_cast<size_t>(levelSelection_)];
            result.subtitle = entry.comment.empty() ? entry.name.c_str() : entry.comment.c_str();
        } else {
            result.subtitle = "back to the main menu";
        }

        // The list lives between the subtitle and the bottom of the panel
        // buildMenu draws. Rows shrink to fit before the window does, and the
        // window shrinks before anything is allowed to spill off the panel.
        const float listTop = screenHeight * 0.36f;
        const float listHeight = std::max(screenHeight * 0.90f - listTop, 1.0f);
        constexpr float kRowMin = 30.0f;
        constexpr float kRowMax = 58.0f;
        const int fits = std::max(static_cast<int>((listHeight + gap) / (kRowMin + gap)), 2);
        const int shown = std::clamp(std::min(levelCount, kMaxLevelRows), 0, fits - 1);
        const int rows = shown + 1;   // + back
        const float rowHeight =
            std::clamp((listHeight - gap * static_cast<float>(rows - 1)) /
                           static_cast<float>(rows),
                       kRowMin, kRowMax);

        // Centred on the cursor rather than scrolled by it, so the window is a
        // function of the selection alone and the view stays pure.
        int first = 0;
        if (levelCount > shown) {
            first = std::clamp(levelSelection_ - (shown - 1) / 2, 0, levelCount - shown);
        }
        result.firstLevelShown = first;
        result.levelsShown = shown;

        for (int i = 0; i < shown; ++i) {
            const int index = first + i;
            MenuButtonView& button = result.buttons[static_cast<size_t>(i)];
            button.action = MenuAction::Start;
            const LevelEntry& level = levels_[static_cast<size_t>(index)];
            button.label = level.name.c_str();
            if (level.completions > 0) {
                if (level.bestSeconds >= 0) {
                    std::snprintf(button.value.data(), button.value.size(), "best %d:%02d",
                                  level.bestSeconds / 60, level.bestSeconds % 60);
                } else {
                    std::snprintf(button.value.data(), button.value.size(), "%s", "complete");
                }
            }
            button.levelIndex = index;
            button.bounds = {left, listTop + static_cast<float>(i) * (rowHeight + gap),
                             buttonWidth, rowHeight};
            button.selected = index == levelSelection_;
        }
        MenuButtonView& back = result.buttons[static_cast<size_t>(shown)];
        back.action = MenuAction::Back;
        back.label = "back";
        back.bounds = {left, listTop + static_cast<float>(shown) * (rowHeight + gap), buttonWidth,
                       rowHeight};
        back.selected = levelSelection_ >= levelCount;
        result.buttonCount = rows;
        return result;
    }

    if (screen_ == MenuScreen::Controls) {
        const int actionCount = static_cast<int>(kPlayerActionCount);
        result.title = "controls";
        result.subtitle = capturingBinding_ ? "press a key or mouse button; escape cancels"
                                            : "choose an action to rebind";
        result.controlCount = actionCount;

        const float listTop = screenHeight * 0.36f;
        const float listHeight = std::max(screenHeight * 0.895f - listTop, 1.0f);
        const float rowGap = std::clamp(screenHeight * 0.008f, 3.0f, 10.0f);
        const int fits = std::max(static_cast<int>((listHeight + rowGap) / (30.0f + rowGap)), 3);
        const int shown = std::clamp(std::min(actionCount, 7), 1, fits - 2);
        const int rows = shown + 2;
        const float rowHeight =
            std::clamp((listHeight - rowGap * static_cast<float>(rows - 1)) /
                           static_cast<float>(rows),
                       18.0f, 52.0f);
        int first = 0;
        if (actionCount > shown) {
            const int focus = std::min(controlSelection_, actionCount - 1);
            first = std::clamp(focus - (shown - 1) / 2, 0, actionCount - shown);
        }
        result.firstControlShown = first;
        result.controlsShown = shown;

        for (int i = 0; i < shown; ++i) {
            const int actionIndex = first + i;
            const PlayerAction action = static_cast<PlayerAction>(actionIndex);
            MenuButtonView& button = result.buttons[static_cast<size_t>(i)];
            button.label = playerActionLabel(action);
            button.actionIndex = actionIndex;
            button.bounds = {left, listTop + static_cast<float>(i) * (rowHeight + rowGap),
                             buttonWidth, rowHeight};
            button.selected = actionIndex == controlSelection_;
            if (button.selected && capturingBinding_) {
                std::snprintf(button.value.data(), button.value.size(), "%s", "press input");
            } else {
                formatInputBinding(editedSettings_.bindings[action], button.value.data(),
                                   button.value.size());
            }
        }
        MenuButtonView& reset = result.buttons[static_cast<size_t>(shown)];
        reset.label = "reset defaults";
        reset.actionIndex = actionCount;
        reset.bounds = {left, listTop + static_cast<float>(shown) * (rowHeight + rowGap),
                        buttonWidth, rowHeight};
        reset.selected = controlSelection_ == actionCount;

        MenuButtonView& back = result.buttons[static_cast<size_t>(shown + 1)];
        back.label = "back";
        back.actionIndex = actionCount + 1;
        back.bounds = {left, listTop + static_cast<float>(shown + 1) * (rowHeight + rowGap),
                       buttonWidth, rowHeight};
        back.selected = controlSelection_ == actionCount + 1;
        result.buttonCount = rows;
        return result;
    }

    switch (screen_) {
        case MenuScreen::Main:
            result.title = "descent";
            result.subtitle = "into the dark";
            break;
        case MenuScreen::Pause:
            result.title = "paused";
            result.subtitle = "the mine is waiting";
            break;
        case MenuScreen::Settings:
            result.title = "settings";
            result.subtitle = "left and right adjust";
            break;
        case MenuScreen::Controls: return result;
        case MenuScreen::DisplayConfirmation:
            result.title = "display changed";
            result.subtitle = "keep these display settings?";
            break;
        case MenuScreen::Summary:
            result.title = won_ ? "mine destroyed" : "you are dead";
            result.subtitle = won_ ? "run complete" : "run ended";
            break;
        case MenuScreen::LevelSelect:   // handled above
        case MenuScreen::Hidden: return result;
    }

    const auto [definitions, count] = buttonsFor(screen_);
    result.buttonCount = count;
    const bool settingsScreen = screen_ == MenuScreen::Settings;
    const float rowGap = settingsScreen ? std::clamp(screenHeight * 0.008f, 3.0f, 10.0f) : gap;
    const float buttonHeight =
        settingsScreen
            ? std::clamp((screenHeight * 0.54f - static_cast<float>(count - 1) * rowGap) /
                             static_cast<float>(count),
                         18.0f, 56.0f)
            : std::clamp(screenHeight * 0.065f, 42.0f, 64.0f);
    const float blockHeight = count * buttonHeight + (count - 1) * rowGap;
    const float blockCenter =
        settingsScreen ? screenHeight * 0.51f
                       : screenHeight * (screen_ == MenuScreen::Summary ? 0.70f : 0.64f);
    const float top = blockCenter - blockHeight * 0.5f;

    for (int i = 0; i < count; ++i) {
        MenuButtonView& button = result.buttons[static_cast<size_t>(i)];
        button.action = definitions[i].action;
        button.label = definitions[i].label;
        button.bounds = {left, top + i * (buttonHeight + rowGap), buttonWidth, buttonHeight};
        button.selected = i == selection_;
        if (screen_ != MenuScreen::Settings) continue;
        switch (i) {
            case 0:
                std::snprintf(button.value.data(), button.value.size(), "%d%%",
                              static_cast<int>(std::round(editedSettings_.masterVolume * 100.0f)));
                break;
            case 1:
                std::snprintf(button.value.data(), button.value.size(), "%.1fx",
                              editedSettings_.mouseSensitivity);
                break;
            case 2:
                std::snprintf(button.value.data(), button.value.size(), "%.0f deg",
                              editedSettings_.firstPersonFov);
                break;
            case 3:
                std::snprintf(button.value.data(), button.value.size(), "%.0f deg",
                              editedSettings_.thirdPersonFov);
                break;
            case 4:
                std::snprintf(button.value.data(), button.value.size(), "%s",
                              editedSettings_.invertMouseY ? "on" : "off");
                break;
            case 5:
                std::snprintf(button.value.data(), button.value.size(), "%s",
                              editedSettings_.fullscreen ? "on" : "off");
                break;
            case 6:
                std::snprintf(button.value.data(), button.value.size(), "%d x %d",
                              editedSettings_.windowWidth, editedSettings_.windowHeight);
                break;
            case 7:
                std::snprintf(button.value.data(), button.value.size(), "%s",
                              editedSettings_.vsync ? "on" : "off");
                break;
            default: break;
        }
    }
    return result;
}

}
