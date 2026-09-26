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
    // What "play" opens: the campaign or the other maps, each a level list.
    LevelGroups,
    LevelSelect,
    Pause,
    // The settings root is a signpost and holds no values of its own; the
    // three below are where they live. They are split because one flat screen
    // of twelve rows was a list to be read rather than a menu to be used.
    Settings,
    SettingsVideo,
    SettingsAudio,
    SettingsGame,
    Controls,
    DisplayConfirmation,
    Summary,
    // A level's scripted tutorial, with the game stopped behind it.
    Tutorial,
};

// Whether this screen edits settings values, which is what makes leaving it an
// apply. The root is not one of them: there is nothing on it to change.
inline bool editsSettings(MenuScreen screen) {
    return screen == MenuScreen::SettingsVideo || screen == MenuScreen::SettingsAudio ||
           screen == MenuScreen::SettingsGame;
}

enum class MenuAction {
    None,
    ChooseLevel,   // main menu "play": open the campaign / other maps choice
    ChooseCampaign,
    ChooseOtherMaps,
    OpenSettings,
    OpenVideoSettings,
    OpenAudioSettings,
    OpenGameSettings,
    OpenControls,
    // Apply and stay, so a display change can be seen and adjusted again
    // without walking back in. Leaving a settings screen applies too - see
    // MenuAction::Back - so this is the discoverable half of that, not the
    // only way to commit.
    ApplySettings,
    KeepDisplay,
    RevertDisplay,
    Start,         // start selectedLevel()
    Back,          // one screen up: level list to groups, groups to main menu
    Resume,
    Restart,
    MainMenu,
    Quit,
    // The okay button on a tutorial pane: the one thing that takes it down.
    AcknowledgeTutorial,
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
    // Wheel notches this frame, positive away from the player. A scrolling
    // list moves its window by it; every other screen ignores it.
    float scroll = 0.0f;
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
    // Non-zero on a scroll arrow: rows it moves the list's window by, negative
    // for up. Such a button is mouse-only - it indexes nothing, and the
    // keyboard cursor never lands on it.
    int scrollRows = 0;
    // False on a scroll arrow with nothing left to scroll. The arrow stays
    // where it is so the strip does not jump; it just does nothing.
    bool enabled = true;
};

// The level list is as long as the directory is, and a screen is not. Eight
// rows plus "back" is what fits at the sizes this game is played at; a longer
// list shows a window of that size and a pair of scroll arrows beside it.
inline constexpr int kMaxLevelRows = 8;
// The scrolling lists are the longest screens now that settings has been split
// three ways: eight level rows, "back", and the two arrows. The widest fixed
// menu is video, at seven.
inline constexpr int kMaxMenuButtons = 11;

struct MenuView {
    MenuScreen screen = MenuScreen::Hidden;
    const char* title = "";
    const char* subtitle = "";
    std::array<MenuButtonView, kMaxMenuButtons> buttons{};
    int buttonCount = 0;
    bool won = false;
    const char* reason = "";
    const char* recordNote = "";
    int elapsedSeconds = 0;
    int robotsDestroyed = 0;
    int robotsTotal = 0;
    // Level list only, and only worth drawing when the list does not fit:
    // which slice of the open group is on screen.
    int levelCount = 0;
    int firstLevelShown = 0;
    int levelsShown = 0;
    int confirmationSeconds = 0;
    int controlCount = 0;
    int firstControlShown = 0;
    int controlsShown = 0;

    // Tutorial only. The strings come from the level's script and live as long
    // as it is loaded, like the level names above. The clip and the frame do
    // not come from the menu at all: the pane's clock is the application's and
    // the menu has no access to it, so whoever drives the frame loop fills
    // these in after asking for the view. They are ignored on every other
    // screen.
    const char* tutorialTitle = "";
    const char* tutorialText = "";
    int tutorialClip = 0;
    int tutorialFrame = 0;
    float tutorialAlpha = 0.0f;
};

// The two lists "play" leads to. The campaign is the first campaignCount
// levels handed to PlayerMenu::setLevels, the other maps the rest.
enum class LevelGroup {
    Campaign,
    Other,
};

class PlayerMenu {
public:
    // The levels the lists offer, campaign first: levels[0, campaignCount) are
    // the campaign and the rest are the other maps. Set once at startup; the
    // menu keeps them and hands out pointers into them, so the strings must
    // outlive every view.
    void setLevels(std::vector<LevelEntry> levels, int campaignCount = 0);
    const std::vector<LevelEntry>& levels() const { return levels_; }
    void setLevelRecord(const std::string& path, int completions, int bestSeconds);
    // Which level the last Start was for, or -1 when the list is empty. Index
    // into levels(), not into the group it was picked from.
    int selectedLevel() const { return chosenLevel_; }
    // Points the cursor at the level already loaded, so the list opens on it.
    void setSelectedLevel(int index);

    void hide();
    void showMain();
    // Campaign or other maps. The cursor starts on the group last opened, or
    // the one holding the loaded level.
    void showLevelGroups();
    // One group's list, with the cursor on the loaded level if it is in it.
    void showLevelSelect(LevelGroup group);
    // The list holding the loaded level.
    void showLevelSelect();
    void showPause();
    void showSettings(const PlayerSettings& settings);
    // One of the three value screens, reached from the settings root. The
    // working copy is already seeded by showSettings, so these only navigate.
    void showSettingsScreen(MenuScreen screen);
    void showControls();
    void closeControls();
    void showDisplayConfirmation(int seconds);
    // Back out of a value screen to the settings root, or off the root to
    // whichever menu opened it.
    void closeSettings();
    void showTutorial(const char* title, const char* text);
    void setConfirmationSeconds(int seconds);
    void showSummary(bool won, const char* reason, float elapsed, int robotsDestroyed,
                     int robotsTotal, const char* recordNote = "");

    MenuAction update(const MenuInput& input, int width, int height);
    MenuView view(int width, int height) const;
    MenuScreen screen() const { return screen_; }
    const PlayerSettings& editedSettings() const { return editedSettings_; }
    bool capturingBinding() const { return capturingBinding_; }

private:
    int buttonCount() const;
    // How many of this screen's rows carry a value that left and right adjust.
    // They are always the first rows, so a row index below this is a value and
    // anything at or above it is a button.
    static int valueRows(MenuScreen screen);
    void adjustSetting(MenuScreen screen, int index, int direction);
    LevelGroup groupOf(int level) const;
    // The open group's slice of levels_.
    int groupBegin() const;
    int groupSize() const;

    MenuScreen screen_ = MenuScreen::Hidden;
    int selection_ = 0;
    std::vector<LevelEntry> levels_;
    int campaignCount_ = 0;
    LevelGroup group_ = LevelGroup::Other;
    // Kept apart from selection_ so that leaving the list and coming back puts
    // the cursor on the level that was picked rather than on the first row.
    // Counted from the start of the open group, like levelScroll_.
    int levelSelection_ = 0;
    int controlSelection_ = 0;
    // The first row of each list's window. Kept apart from the cursor, so a
    // row under the pointer cannot pull the list out from under the pointer:
    // hovering moves the cursor, and only the keyboard, the wheel and the
    // arrows move the window.
    int levelScroll_ = 0;
    int controlScroll_ = 0;
    int chosenLevel_ = -1;
    bool won_ = false;
    const char* reason_ = "";
    const char* recordNote_ = "";
    int elapsedSeconds_ = 0;
    int robotsDestroyed_ = 0;
    int robotsTotal_ = 0;
    MenuScreen settingsParent_ = MenuScreen::Main;
    PlayerSettings editedSettings_;
    const char* tutorialTitle_ = "";
    const char* tutorialText_ = "";
    int confirmationSeconds_ = 0;
    bool capturingBinding_ = false;
};

}
