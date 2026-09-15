#pragma once

#include "game/level_list.h"
#include "game/settings.h"

#include <glm/vec2.hpp>

#include <array>
#include <string>
#include <vector>

namespace dvk {

enum class MenuScreen {
    Hidden,
    Main,
    LevelSelect,
    Pause,
    Settings,
    Controls,
    DisplayConfirmation,
    Summary,
};

enum class MenuAction {
    None,
    ChooseLevel,   // main menu "play": open the level list
    OpenSettings,
    OpenControls,
    ApplySettings,
    KeepDisplay,
    RevertDisplay,
    Start,         // start selectedLevel()
    Back,          // level list "back": return to the main menu
    Resume,
    Restart,
    MainMenu,
    Quit,
};

struct MenuInput {
    bool previous = false;
    bool next = false;
    bool decrease = false;
    bool increase = false;
    bool confirm = false;
    bool pointerPressed = false;
    bool cancel = false;
    bool bindingPressed = false;
    InputBinding binding;
    glm::vec2 pointer{0.0f};
};

struct MenuRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    bool contains(const glm::vec2& point) const {
        return point.x >= x && point.y >= y && point.x < x + width && point.y < y + height;
    }
};

struct MenuButtonView {
    MenuAction action = MenuAction::None;
    const char* label = "";
    MenuRect bounds;
    bool selected = false;
    std::array<char, 32> value{};
    // >= 0 on a level row: which level of the menu's list it starts.
    int levelIndex = -1;
    int actionIndex = -1;
};

// The level list is as long as the directory is, and a screen is not. Eight
// rows plus "back" is what fits at the sizes this game is played at; a longer
// list scrolls a window of that size around the selection.
inline constexpr int kMaxLevelRows = 8;
inline constexpr int kMaxMenuButtons = kMaxLevelRows + 1;

struct MenuView {
    MenuScreen screen = MenuScreen::Hidden;
    const char* title = "";
    const char* subtitle = "";
    std::array<MenuButtonView, kMaxMenuButtons> buttons{};
    int buttonCount = 0;
    bool won = false;
    const char* reason = "";
    int elapsedSeconds = 0;
    int robotsDestroyed = 0;
    int robotsTotal = 0;
    // Level list only, and only worth drawing when the list does not fit:
    // which slice of it is on screen.
    int levelCount = 0;
    int firstLevelShown = 0;
    int levelsShown = 0;
    int confirmationSeconds = 0;
    int controlCount = 0;
    int firstControlShown = 0;
    int controlsShown = 0;
};

class PlayerMenu {
public:
    // The levels the list offers. Set once at startup; the menu keeps them and
    // hands out pointers into them, so the strings must outlive every view.
    void setLevels(std::vector<LevelEntry> levels);
    const std::vector<LevelEntry>& levels() const { return levels_; }
    // Which level the last Start was for, or -1 when the list is empty. Index
    // into levels().
    int selectedLevel() const { return chosenLevel_; }
    // Points the cursor at the level already loaded, so the list opens on it.
    void setSelectedLevel(int index);

    void hide();
    void showMain();
    void showLevelSelect();
    void showPause();
    void showSettings(const PlayerSettings& settings);
    void showControls();
    void closeControls();
    void showDisplayConfirmation(int seconds);
    void closeSettings();
    void setConfirmationSeconds(int seconds);
    void showSummary(bool won, const char* reason, float elapsed, int robotsDestroyed,
                     int robotsTotal);

    MenuAction update(const MenuInput& input, int width, int height);
    MenuView view(int width, int height) const;
    MenuScreen screen() const { return screen_; }
    const PlayerSettings& editedSettings() const { return editedSettings_; }
    bool capturingBinding() const { return capturingBinding_; }

private:
    int buttonCount() const;
    void adjustSetting(int index, int direction);

    MenuScreen screen_ = MenuScreen::Hidden;
    int selection_ = 0;
    std::vector<LevelEntry> levels_;
    // Kept apart from selection_ so that leaving the list and coming back puts
    // the cursor on the level that was picked rather than on the first row.
    int levelSelection_ = 0;
    int controlSelection_ = 0;
    int chosenLevel_ = -1;
    bool won_ = false;
    const char* reason_ = "";
    int elapsedSeconds_ = 0;
    int robotsDestroyed_ = 0;
    int robotsTotal_ = 0;
    MenuScreen settingsParent_ = MenuScreen::Main;
    PlayerSettings editedSettings_;
    int confirmationSeconds_ = 0;
    bool capturingBinding_ = false;
};

}
