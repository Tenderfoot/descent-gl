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

// A slot's row is its own index, the way a level group's is.
constexpr std::array<ButtonDefinition, 4> kSaveSlotButtons{{
    {MenuAction::ChooseSlot, "slot 1"},
    {MenuAction::ChooseSlot, "slot 2"},
    {MenuAction::ChooseSlot, "slot 3"},
    {MenuAction::Back, "back"},
}};

// Rows 0-2 are Difficulty in order, and ultra-hard follows them.
constexpr std::array<ButtonDefinition, 5> kNewSlotButtons{{
    {MenuAction::CreateSlot, "easy"},
    {MenuAction::CreateSlot, "normal"},
    {MenuAction::CreateSlot, "hard"},
    {MenuAction::CreateSlot, "ultra-hard"},
    {MenuAction::Back, "back"},
}};

constexpr std::array<const char*, 5> kNewSlotSubtitles{
    "the most supplies in every mine",
    "fewer supplies",
    "the fewest supplies",
    "hard, and dying or leaving a level deletes this save",
    "back to the save slots",
};

// Row order matches LevelGroup, so a group is its own row.
constexpr std::array<ButtonDefinition, 4> kLevelGroupButtons{{
    {MenuAction::ChooseCampaign, "campaign"},
    {MenuAction::ChooseOtherMaps, "other maps"},
    {MenuAction::DeleteSlot, "delete save"},
    {MenuAction::Back, "back"},
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

// The summary of a run that took its ultra-hard slot with it.
constexpr std::array<ButtonDefinition, 1> kSummaryFinalButtons{{
    {MenuAction::MainMenu, "main menu"},
}};

// Labelled by showConfirm, so these are placeholders.
constexpr std::array<ButtonDefinition, 2> kConfirmButtons{{
    {MenuAction::ConfirmYes, "yes"},
    {MenuAction::ConfirmNo, "no"},
}};

// The settings root holds no values, only the way into the screens that do.
constexpr std::array<ButtonDefinition, 5> kSettingsButtons{{
    {MenuAction::OpenVideoSettings, "video"},
    {MenuAction::OpenAudioSettings, "audio"},
    {MenuAction::OpenGameSettings, "game"},
    {MenuAction::OpenControls, "controls"},
    {MenuAction::Back, "back"},
}};

// The three value screens. Their adjustable rows come first and in the order
// PlayerMenu::adjustSetting reads them, which is what lets a row index stand
// for a setting without a table mapping the two.
//
// Field of view is video and not game: it is the vertical angle the projection
// matrix is built from, and it belongs beside the other things that decide what
// the window shows.
constexpr std::array<ButtonDefinition, 7> kVideoButtons{{
    {MenuAction::None, "fullscreen"},
    {MenuAction::None, "window size"},
    {MenuAction::None, "vsync"},
    {MenuAction::None, "1st person fov"},
    {MenuAction::None, "3rd person fov"},
    {MenuAction::ApplySettings, "apply"},
    {MenuAction::Back, "back"},
}};

constexpr std::array<ButtonDefinition, 5> kAudioButtons{{
    {MenuAction::None, "master volume"},
    {MenuAction::None, "music volume"},
    {MenuAction::None, "effects volume"},
    {MenuAction::ApplySettings, "apply"},
    {MenuAction::Back, "back"},
}};

constexpr std::array<ButtonDefinition, 6> kGameButtons{{
    {MenuAction::None, "mouse sensitivity"},
    {MenuAction::None, "invert mouse y"},
    {MenuAction::None, "tutorials"},
    {MenuAction::None, "weapon hitboxes"},
    {MenuAction::ApplySettings, "apply"},
    {MenuAction::Back, "back"},
}};

// A tutorial pane has one button, because it has one thing the player can do.
constexpr std::array<ButtonDefinition, 1> kTutorialButtons{{
    {MenuAction::AcknowledgeTutorial, "okay"},
}};

// Where "controls" sits in the root table, so closing that screen can put the
// cursor back on the row that opened it without counting rows by hand.
constexpr int kSettingsControlsRow = 3;

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

std::pair<const ButtonDefinition*, int> buttonsFor(MenuScreen screen, bool summaryRetry) {
    switch (screen) {
        case MenuScreen::Main: return {kMainButtons.data(), static_cast<int>(kMainButtons.size())};
        case MenuScreen::SaveSlots:
            return {kSaveSlotButtons.data(), static_cast<int>(kSaveSlotButtons.size())};
        case MenuScreen::NewSlot:
            return {kNewSlotButtons.data(), static_cast<int>(kNewSlotButtons.size())};
        case MenuScreen::Confirm:
            return {kConfirmButtons.data(), static_cast<int>(kConfirmButtons.size())};
        case MenuScreen::LevelGroups:
            return {kLevelGroupButtons.data(), static_cast<int>(kLevelGroupButtons.size())};
        case MenuScreen::Pause:
            return {kPauseButtons.data(), static_cast<int>(kPauseButtons.size())};
        case MenuScreen::Settings:
            return {kSettingsButtons.data(), static_cast<int>(kSettingsButtons.size())};
        case MenuScreen::SettingsVideo:
            return {kVideoButtons.data(), static_cast<int>(kVideoButtons.size())};
        case MenuScreen::SettingsAudio:
            return {kAudioButtons.data(), static_cast<int>(kAudioButtons.size())};
        case MenuScreen::SettingsGame:
            return {kGameButtons.data(), static_cast<int>(kGameButtons.size())};
        case MenuScreen::Tutorial:
            return {kTutorialButtons.data(), static_cast<int>(kTutorialButtons.size())};
        case MenuScreen::Controls:
            return {nullptr, static_cast<int>(kPlayerActionCount) + 2};
        case MenuScreen::DisplayConfirmation:
            return {kDisplayConfirmationButtons.data(),
                    static_cast<int>(kDisplayConfirmationButtons.size())};
        case MenuScreen::Summary:
            if (!summaryRetry) {
                return {kSummaryFinalButtons.data(),
                        static_cast<int>(kSummaryFinalButtons.size())};
            }
            return {kSummaryButtons.data(), static_cast<int>(kSummaryButtons.size())};
        case MenuScreen::LevelSelect:   // built from the level list, not a table
        case MenuScreen::Hidden: return {nullptr, 0};
    }
    return {nullptr, 0};
}

// Both scrolling lists - the levels and the controls - are a window of rows
// with one or two fixed rows pinned under it, and a strip of scroll arrows
// beside the window once the list is longer than it. Where all of that sits is
// worked out here rather than inside view(), because update() needs the same
// numbers: the cursor and the window have to agree on what is on screen.
struct ListSpec {
    int count = 0;           // rows the list can scroll through
    int fixedRows = 1;       // rows pinned under the window
    int maxWindow = 1;       // most list rows to put on screen
    int minWindow = 0;       // fewest, once the screen has had its say
    float bottom = 0.90f;    // fraction of the screen height the list stops at
    float gap = 0.0f;
    float fitRow = 30.0f;    // the row height the fit is measured in
    float rowMin = 30.0f;
    float rowMax = 58.0f;
};

struct ListLayout {
    int shown = 0;
    int rows = 0;            // those plus the fixed ones
    int page = 1;            // rows one press of a scroll arrow moves
    float top = 0.0f;
    float left = 0.0f;
    float fullWidth = 0.0f;  // a fixed row spans the whole column
    float rowWidth = 0.0f;   // a list row gives up the strip's width
    float rowHeight = 0.0f;
    float gap = 0.0f;
    float stripLeft = 0.0f;
    float stripWidth = 0.0f;
    bool scrollable = false;
};

ListLayout layoutList(const ListSpec& spec, int width, int height) {
    const float screenWidth = static_cast<float>(std::max(width, 1));
    const float screenHeight = static_cast<float>(std::max(height, 1));
    ListLayout layout;
    layout.gap = spec.gap;
    layout.fullWidth = std::clamp(screenWidth * 0.32f, 260.0f, 600.0f);
    layout.rowWidth = layout.fullWidth;
    layout.left = (screenWidth - layout.fullWidth) * 0.5f;
    layout.top = screenHeight * 0.36f;
    // Rows shrink to fit before the window does, and the window shrinks before
    // anything is allowed to spill off the panel buildMenu draws.
    const float listHeight = std::max(screenHeight * spec.bottom - layout.top, 1.0f);
    const int fits =
        std::max(static_cast<int>((listHeight + layout.gap) / (spec.fitRow + layout.gap)),
                 spec.fixedRows + 1);
    layout.shown = std::clamp(std::min(spec.count, spec.maxWindow), spec.minWindow,
                              fits - spec.fixedRows);
    layout.rows = layout.shown + spec.fixedRows;
    layout.rowHeight =
        std::clamp((listHeight - layout.gap * static_cast<float>(layout.rows - 1)) /
                       static_cast<float>(layout.rows),
                   spec.rowMin, spec.rowMax);
    layout.scrollable = layout.shown > 0 && spec.count > layout.shown;
    // A press moves by a window less one row, so the row a reader stopped on
    // is still there after the jump.
    layout.page = std::max(layout.shown - 1, 1);
    if (layout.scrollable) {
        layout.stripWidth = std::clamp(layout.fullWidth * 0.11f, 28.0f, 48.0f);
        layout.rowWidth = layout.fullWidth - layout.stripWidth - layout.gap * 0.5f;
        layout.stripLeft = layout.left + layout.fullWidth - layout.stripWidth;
    }
    return layout;
}

ListLayout levelListLayout(int count, int width, int height) {
    ListSpec spec;
    spec.count = count;
    spec.fixedRows = 1;   // back
    spec.maxWindow = kMaxLevelRows;
    spec.minWindow = 0;   // an empty directory is a list of nothing
    spec.bottom = 0.90f;
    spec.gap = std::clamp(static_cast<float>(std::max(height, 1)) * 0.018f, 10.0f, 18.0f);
    spec.rowMin = 30.0f;
    spec.rowMax = 66.0f;   // two lines: the name and its records
    return layoutList(spec, width, height);
}

ListLayout controlListLayout(int count, int width, int height) {
    ListSpec spec;
    spec.count = count;
    spec.fixedRows = 2;   // reset defaults, back
    spec.maxWindow = 7;
    spec.minWindow = 1;
    spec.bottom = 0.895f;
    spec.gap = std::clamp(static_cast<float>(std::max(height, 1)) * 0.008f, 3.0f, 10.0f);
    spec.rowMin = 18.0f;
    spec.rowMax = 52.0f;
    return layoutList(spec, width, height);
}

// The strip beside a scrolling list: an up arrow over the top half of the
// window and a down arrow over the bottom half. Two targets that large are
// what makes a long list navigable with a mouse alone; they move the window
// without moving the cursor, so nothing shifts under the pointer.
void addScrollArrows(MenuView& result, const ListLayout& layout, int first, int count) {
    if (!layout.scrollable) return;
    const float span = static_cast<float>(layout.shown - 1) * (layout.rowHeight + layout.gap) +
                       layout.rowHeight;
    const float half = (span - layout.gap * 0.5f) * 0.5f;
    MenuButtonView& up = result.buttons[static_cast<size_t>(result.buttonCount++)];
    up.scrollRows = -layout.page;
    up.enabled = first > 0;
    up.bounds = {layout.stripLeft, layout.top, layout.stripWidth, half};
    MenuButtonView& down = result.buttons[static_cast<size_t>(result.buttonCount++)];
    down.scrollRows = layout.page;
    down.enabled = first + layout.shown < count;
    down.bounds = {layout.stripLeft, layout.top + span - half, layout.stripWidth, half};
}

}

void PlayerMenu::setLevels(std::vector<LevelEntry> levels, int campaignCount) {
    levels_ = std::move(levels);
    campaignCount_ = std::clamp(campaignCount, 0, static_cast<int>(levels_.size()));
    // A level's comment is written for whoever opens the file - mine01's is a
    // paragraph about where the keys are. The list wants a blurb, so keep the
    // first sentence and drop the rest. The renderer still cuts what does not
    // fit; this is about what is worth showing, not about what fits.
    for (LevelEntry& entry : levels_) {
        const size_t stop = entry.comment.find(". ");
        if (stop != std::string::npos) entry.comment.resize(stop + 1);
    }
    const int count = static_cast<int>(levels_.size());
    chosenLevel_ = count == 0 ? -1 : std::clamp(chosenLevel_, 0, count - 1);
    group_ = groupOf(chosenLevel_);
    const int size = groupSize();
    levelSelection_ = std::clamp(levelSelection_, 0, size);
    levelScroll_ = std::clamp(levelScroll_, 0, std::max(size - 1, 0));
}

LevelGroup PlayerMenu::groupOf(int level) const {
    // Nothing chosen goes with the campaign when there is one: it is where a
    // player who has not picked anything should be pointed.
    if (level < 0) return campaignCount_ > 0 ? LevelGroup::Campaign : LevelGroup::Other;
    return level < campaignCount_ ? LevelGroup::Campaign : LevelGroup::Other;
}

int PlayerMenu::groupBegin() const {
    return group_ == LevelGroup::Campaign ? 0 : campaignCount_;
}

int PlayerMenu::groupSize() const {
    return group_ == LevelGroup::Campaign ? campaignCount_
                                          : static_cast<int>(levels_.size()) - campaignCount_;
}

void PlayerMenu::setSelectedLevel(int index) {
    if (levels_.empty()) {
        levelSelection_ = 0;
        chosenLevel_ = -1;
        return;
    }
    chosenLevel_ = std::clamp(index, 0, static_cast<int>(levels_.size()) - 1);
    group_ = groupOf(chosenLevel_);
    levelSelection_ = chosenLevel_ - groupBegin();
    levelScroll_ = levelSelection_;
}

void PlayerMenu::setLevelRecord(const std::string& path, int runs, int completions,
                                int bestSeconds, int mostRobots) {
    const auto found = std::find_if(levels_.begin(), levels_.end(),
                                    [&](const LevelEntry& level) { return level.path == path; });
    if (found == levels_.end()) return;
    found->runs = std::max(runs, 0);
    found->completions = std::max(completions, 0);
    found->bestSeconds = bestSeconds;
    found->mostRobots = std::max(mostRobots, 0);
}

void PlayerMenu::setSlotSummary(int slot, const char* difficulty, int campaignWon,
                                int campaignTotal) {
    if (slot < 0 || slot >= static_cast<int>(slots_.size())) return;
    SlotSummary& summary = slots_[static_cast<size_t>(slot)];
    summary.used = difficulty != nullptr;
    if (!summary.used) {
        std::snprintf(summary.value.data(), summary.value.size(), "%s", "empty");
        std::snprintf(summary.detail.data(), summary.detail.size(), "%s", "start a new game");
        summary.heading[0] = '\0';
        return;
    }
    std::snprintf(summary.value.data(), summary.value.size(), "%s", difficulty);
    std::snprintf(summary.detail.data(), summary.detail.size(), "campaign %d of %d cleared",
                  campaignWon, campaignTotal);
    std::snprintf(summary.heading.data(), summary.heading.size(), "slot %d   %s", slot + 1,
                  difficulty);
}

void PlayerMenu::hide() {
    screen_ = MenuScreen::Hidden;
    selection_ = 0;
    // A tutorial's strings belong to the loaded script, which is thrown away
    // and rebuilt by every restart and every level change. Nothing should be
    // reading them once the menu is down, and holding them would make that a
    // dangling pointer rather than an empty pane.
    tutorialTitle_ = "";
    tutorialText_ = "";
}

void PlayerMenu::showMain() {
    // Leaving a level is the one teardown that does not go through hide(), and
    // the tutorial strings belong to the script that level loaded. See hide().
    tutorialTitle_ = "";
    tutorialText_ = "";
    screen_ = MenuScreen::Main;
    selection_ = 0;
}

void PlayerMenu::showSaveSlots() {
    screen_ = MenuScreen::SaveSlots;
    // Back on the slot that was last opened, so leaving it and coming back
    // does not walk the cursor to the top.
    selection_ = std::clamp(chosenSlot_, 0, static_cast<int>(slots_.size()) - 1);
}

void PlayerMenu::showNewSlot() {
    screen_ = MenuScreen::NewSlot;
    selection_ = 1;   // normal
}

void PlayerMenu::showConfirm(const char* title, const char* subtitle, const char* yes,
                             const char* no) {
    screen_ = MenuScreen::Confirm;
    selection_ = 1;
    confirmTitle_ = title;
    confirmSubtitle_ = subtitle;
    confirmYes_ = yes;
    confirmNo_ = no;
}

void PlayerMenu::showLevelGroups() {
    // Coming back up from a list lands on the row that opened it; coming from
    // the main menu, group_ is the loaded level's, so confirming all the way
    // down still starts what a plain run starts.
    screen_ = MenuScreen::LevelGroups;
    selection_ = static_cast<int>(group_);
}

void PlayerMenu::showLevelSelect(LevelGroup group) {
    screen_ = MenuScreen::LevelSelect;
    group_ = group;
    // The cursor opens on the level that was last picked if this group holds
    // it, so backing out of a run and coming here again does not start over
    // at the top of the list. The window opens on that row - view() clamps it
    // to the end of the list, and how many rows fit is not known here.
    const int size = groupSize();
    levelSelection_ = chosenLevel_ >= 0 && groupOf(chosenLevel_) == group_
                          ? chosenLevel_ - groupBegin()
                          : 0;
    levelSelection_ = std::clamp(levelSelection_, 0, size);
    levelScroll_ = std::min(levelSelection_, std::max(size - 1, 0));
}

void PlayerMenu::showLevelSelect() {
    showLevelSelect(groupOf(chosenLevel_));
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

void PlayerMenu::showSettingsScreen(MenuScreen screen) {
    if (!editsSettings(screen)) return;
    screen_ = screen;
    selection_ = 0;
}

void PlayerMenu::showControls() {
    screen_ = MenuScreen::Controls;
    controlSelection_ = std::clamp(controlSelection_, 0, static_cast<int>(kPlayerActionCount) + 1);
    controlScroll_ = std::min(controlSelection_, static_cast<int>(kPlayerActionCount) - 1);
    capturingBinding_ = false;
}

void PlayerMenu::closeControls() {
    screen_ = MenuScreen::Settings;
    selection_ = kSettingsControlsRow;
    capturingBinding_ = false;
}

void PlayerMenu::closeSettings() {
    // One step at a time. A value screen goes back to the root with the cursor
    // on the row that opened it, so walking in and out of the three of them
    // does not put the cursor back at the top each time; the root goes back to
    // whichever menu opened it.
    if (editsSettings(screen_)) {
        const int row = screen_ == MenuScreen::SettingsVideo   ? 0
                        : screen_ == MenuScreen::SettingsAudio ? 1
                                                               : 2;
        screen_ = MenuScreen::Settings;
        selection_ = row;
        return;
    }
    screen_ = settingsParent_;
    selection_ = 0;
}

void PlayerMenu::showTutorial(const char* title, const char* text) {
    screen_ = MenuScreen::Tutorial;
    selection_ = 0;
    tutorialTitle_ = title ? title : "";
    tutorialText_ = text ? text : "";
}

void PlayerMenu::setConfirmationSeconds(int seconds) {
    confirmationSeconds_ = std::max(seconds, 0);
}

void PlayerMenu::showSummary(bool won, const char* reason, float elapsed, int robotsDestroyed,
                             int robotsTotal, const char* recordNote, bool retry) {
    screen_ = MenuScreen::Summary;
    selection_ = 0;
    summaryRetry_ = retry;
    won_ = won;
    reason_ = reason;
    recordNote_ = recordNote;
    elapsedSeconds_ = std::max(0, static_cast<int>(elapsed));
    robotsDestroyed_ = robotsDestroyed;
    robotsTotal_ = robotsTotal;
}

// How many things the cursor can sit on. For the level list that is one per
// level in the open group plus "back", which is more than the rows on screen
// when the list is long: the cursor moves through the whole list and the
// window follows it.
int PlayerMenu::buttonCount() const {
    if (screen_ == MenuScreen::LevelSelect) return groupSize() + 1;
    if (screen_ == MenuScreen::Controls) return static_cast<int>(kPlayerActionCount) + 2;
    return buttonsFor(screen_, summaryRetry_).second;
}

int PlayerMenu::valueRows(MenuScreen screen) {
    switch (screen) {
        case MenuScreen::SettingsVideo: return 5;
        case MenuScreen::SettingsAudio: return 3;
        case MenuScreen::SettingsGame: return 4;
        default: return 0;
    }
}

void PlayerMenu::adjustSetting(MenuScreen screen, int index, int direction) {
    if (direction == 0) return;
    const auto step = [&](float value, float by, float low, float high) {
        return std::clamp(value + static_cast<float>(direction) * by, low, high);
    };

    switch (screen) {
        case MenuScreen::SettingsVideo:
            switch (index) {
                case 0: editedSettings_.fullscreen = !editedSettings_.fullscreen; break;
                case 1: {
                    // The authored sizes are a ring rather than a range, so
                    // this steps to the neighbour of whichever one the current
                    // size is nearest - which is also how a size loaded from a
                    // settings file that names no listed size gets onto one.
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
                    editedSettings_.windowWidth =
                        kWindowSizes[static_cast<size_t>(closest)].first;
                    editedSettings_.windowHeight =
                        kWindowSizes[static_cast<size_t>(closest)].second;
                    break;
                }
                case 2: editedSettings_.vsync = !editedSettings_.vsync; break;
                case 3:
                    editedSettings_.firstPersonFov =
                        step(editedSettings_.firstPersonFov, 5.0f, kMinVerticalFov,
                             kMaxVerticalFov);
                    break;
                case 4:
                    editedSettings_.thirdPersonFov =
                        step(editedSettings_.thirdPersonFov, 5.0f, kMinVerticalFov,
                             kMaxVerticalFov);
                    break;
                default: break;
            }
            break;
        case MenuScreen::SettingsAudio:
            switch (index) {
                case 0:
                    editedSettings_.masterVolume =
                        step(editedSettings_.masterVolume, 0.05f, 0.0f, 1.0f);
                    break;
                case 1:
                    editedSettings_.musicVolume =
                        step(editedSettings_.musicVolume, 0.05f, 0.0f, 1.0f);
                    break;
                case 2:
                    editedSettings_.effectsVolume =
                        step(editedSettings_.effectsVolume, 0.05f, 0.0f, 1.0f);
                    break;
                default: break;
            }
            break;
        case MenuScreen::SettingsGame:
            switch (index) {
                case 0:
                    editedSettings_.mouseSensitivity =
                        step(editedSettings_.mouseSensitivity, 0.1f, 0.2f, 3.0f);
                    break;
                case 1: editedSettings_.invertMouseY = !editedSettings_.invertMouseY; break;
                case 2: editedSettings_.tutorials = !editedSettings_.tutorials; break;
                case 3:
                    editedSettings_.weaponHitboxes = !editedSettings_.weaponHitboxes;
                    break;
                default: break;
            }
            break;
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

    // A scrolling list keeps its window and its cursor apart. Hovering a row
    // moves the cursor; the wheel and the arrows move the window. Whichever
    // moved last is the one that wins when the two disagree, so a row never
    // slides out from under the pointer that is pointing at it.
    const int listCount = levelList  ? groupSize()
                          : controlList ? static_cast<int>(kPlayerActionCount)
                                        : 0;
    int* const scroll = levelList ? &levelScroll_ : (controlList ? &controlScroll_ : nullptr);
    const int shown = !scroll ? 0
                      : levelList ? levelListLayout(listCount, width, height).shown
                                  : controlListLayout(listCount, width, height).shown;
    const int maxScroll = std::max(listCount - shown, 0);
    bool windowMoved = false;
    if (scroll && input.scroll != 0.0f) {
        *scroll = std::clamp(*scroll - static_cast<int>(std::lround(input.scroll)), 0, maxScroll);
        windowMoved = true;
    }

    const MenuView current = view(width, height);
    for (int i = 0; i < current.buttonCount; ++i) {
        const MenuButtonView& button = current.buttons[static_cast<size_t>(i)];
        // A scroll arrow indexes nothing: pointing at one must not change what
        // is selected, or the list would pick a row on the way to scrolling.
        if (button.scrollRows != 0) continue;
        if (!button.bounds.contains(input.pointer)) continue;
        // On the level list a button is not its own index: the rows show a
        // window into the group, and "back" sits past the end of it.
        if (levelList) {
            cursor = button.levelIndex >= 0 ? button.levelIndex - groupBegin() : count - 1;
        }
        else if (controlList) cursor = button.actionIndex;
        else cursor = i;
    }

    if (input.previous) {
        cursor = (cursor + count - 1) % count;
        windowMoved = false;
    }
    if (input.next) {
        cursor = (cursor + 1) % count;
        windowMoved = false;
    }

    if (scroll && shown > 0 && listCount > 0) {
        if (windowMoved) {
            // Scrolling carries the cursor along, so what is highlighted is
            // always a row the list is showing. The fixed rows underneath keep
            // the cursor wherever the window goes.
            if (cursor < listCount) cursor = std::clamp(cursor, *scroll, *scroll + shown - 1);
        } else if (cursor < listCount) {
            // The keyboard drags the window, and only far enough to bring the
            // row it moved to back on screen.
            *scroll = std::clamp(std::clamp(*scroll, cursor - shown + 1, cursor), 0, maxScroll);
        }
    }

    if (selection_ < valueRows(screen_)) {
        if (input.decrease) adjustSetting(screen_, selection_, -1);
        if (input.increase) adjustSetting(screen_, selection_, 1);
    }

    // A slot row and a difficulty row are one action each, told apart by
    // which row it was; the shell asks for that afterwards.
    const auto pick = [&](int row) {
        if (screen_ == MenuScreen::SaveSlots && row < static_cast<int>(slots_.size())) {
            chosenSlot_ = row;
        } else if (screen_ == MenuScreen::NewSlot && row < 4) {
            chosenDifficulty_ = row;
        }
    };

    const MenuView updated = view(width, height);
    if (input.pointerPressed) {
        for (int i = 0; i < updated.buttonCount; ++i) {
            const MenuButtonView& button = updated.buttons[static_cast<size_t>(i)];
            if (!button.bounds.contains(input.pointer)) continue;
            if (button.scrollRows != 0) {
                if (scroll) {
                    *scroll = std::clamp(*scroll + button.scrollRows, 0, maxScroll);
                    if (shown > 0 && cursor < listCount) {
                        cursor = std::clamp(cursor, *scroll, *scroll + shown - 1);
                    }
                }
                return MenuAction::None;
            }
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
            if (i < valueRows(screen_)) {
                adjustSetting(screen_, i, 1);
                return MenuAction::None;
            }
            pick(i);
            return button.action;
        }
    }
    if (input.confirm) {
        if (levelList) {
            if (levelSelection_ >= groupSize()) return MenuAction::Back;
            chosenLevel_ = groupBegin() + levelSelection_;
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
        if (selection_ < valueRows(screen_)) {
            adjustSetting(screen_, selection_, 1);
            return MenuAction::None;
        }
        const auto [definitions, definitionCount] = buttonsFor(screen_, summaryRetry_);
        if (selection_ < definitionCount) {
            pick(selection_);
            return definitions[selection_].action;
        }
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
    const float buttonWidth = std::clamp(screenWidth * 0.32f, 260.0f, 600.0f);
    const float left = (screenWidth - buttonWidth) * 0.5f;
    const float gap = std::clamp(screenHeight * 0.018f, 10.0f, 18.0f);

    if (screen_ == MenuScreen::LevelSelect) {
        const bool campaign = group_ == LevelGroup::Campaign;
        const int begin = groupBegin();
        const int levelCount = groupSize();
        result.title = campaign ? "campaign" : "other maps";
        result.levelCount = levelCount;
        if (levelCount == 0) {
            result.subtitle = campaign ? "no levels in assets/config/campaign.json"
                                       : "no other levels in assets/levels";
        } else if (levelSelection_ < levelCount) {
            const LevelEntry& entry = levels_[static_cast<size_t>(begin + levelSelection_)];
            result.subtitle = entry.comment.empty() ? entry.name.c_str() : entry.comment.c_str();
        } else {
            result.subtitle = "back to campaign or other maps";
        }

        // The list lives between the subtitle and the bottom of the panel
        // buildMenu draws.
        const ListLayout layout = levelListLayout(levelCount, width, height);
        const int shown = layout.shown;
        // The window follows levelScroll_, which nothing but the keyboard, the
        // wheel and the arrows moves. Clamped rather than corrected, so the
        // view stays a pure function of the state.
        const int first = std::clamp(levelScroll_, 0, std::max(levelCount - shown, 0));
        result.firstLevelShown = first;
        result.levelsShown = shown;

        for (int i = 0; i < shown; ++i) {
            const int index = first + i;
            MenuButtonView& button = result.buttons[static_cast<size_t>(i)];
            button.action = MenuAction::Start;
            const LevelEntry& level = levels_[static_cast<size_t>(begin + index)];
            button.label = level.name.c_str();
            if (level.runs == 0 && level.completions == 0) {
                std::snprintf(button.detail.data(), button.detail.size(), "%s",
                              "not attempted");
            } else {
                char best[16] = "";
                if (level.completions > 0 && level.bestSeconds >= 0) {
                    std::snprintf(best, sizeof(best), "best %d:%02d   ", level.bestSeconds / 60,
                                  level.bestSeconds % 60);
                }
                std::snprintf(button.detail.data(), button.detail.size(),
                              "%sattempts %d   successes %d   robots %d", best,
                              std::max(level.runs, level.completions), level.completions,
                              level.mostRobots);
            }
            button.levelIndex = begin + index;
            button.bounds = {layout.left,
                             layout.top + static_cast<float>(i) * (layout.rowHeight + layout.gap),
                             layout.rowWidth, layout.rowHeight};
            button.selected = index == levelSelection_;
        }
        MenuButtonView& back = result.buttons[static_cast<size_t>(shown)];
        back.action = MenuAction::Back;
        back.label = "back";
        back.bounds = {layout.left,
                       layout.top + static_cast<float>(shown) * (layout.rowHeight + layout.gap),
                       layout.fullWidth, layout.rowHeight};
        back.selected = levelSelection_ >= levelCount;
        result.buttonCount = layout.rows;
        addScrollArrows(result, layout, first, levelCount);
        return result;
    }

    if (screen_ == MenuScreen::Controls) {
        const int actionCount = static_cast<int>(kPlayerActionCount);
        result.title = "controls";
        result.subtitle = capturingBinding_ ? "press a key or mouse button; escape cancels"
                                            : "choose an action to rebind";
        result.controlCount = actionCount;

        const ListLayout layout = controlListLayout(actionCount, width, height);
        const int shown = layout.shown;
        const int first = std::clamp(controlScroll_, 0, std::max(actionCount - shown, 0));
        result.firstControlShown = first;
        result.controlsShown = shown;

        for (int i = 0; i < shown; ++i) {
            const int actionIndex = first + i;
            const PlayerAction action = static_cast<PlayerAction>(actionIndex);
            MenuButtonView& button = result.buttons[static_cast<size_t>(i)];
            button.label = playerActionLabel(action);
            button.actionIndex = actionIndex;
            button.bounds = {layout.left,
                             layout.top + static_cast<float>(i) * (layout.rowHeight + layout.gap),
                             layout.rowWidth, layout.rowHeight};
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
        reset.bounds = {layout.left,
                        layout.top + static_cast<float>(shown) * (layout.rowHeight + layout.gap),
                        layout.fullWidth, layout.rowHeight};
        reset.selected = controlSelection_ == actionCount;

        MenuButtonView& back = result.buttons[static_cast<size_t>(shown + 1)];
        back.label = "back";
        back.actionIndex = actionCount + 1;
        back.bounds = {layout.left,
                       layout.top + static_cast<float>(shown + 1) * (layout.rowHeight + layout.gap),
                       layout.fullWidth, layout.rowHeight};
        back.selected = controlSelection_ == actionCount + 1;
        result.buttonCount = layout.rows;
        addScrollArrows(result, layout, first, actionCount);
        return result;
    }

    switch (screen_) {
        case MenuScreen::Main:
            result.title = "descent";
            result.subtitle = "";
            break;
        case MenuScreen::SaveSlots:
            result.title = "play";
            result.subtitle = "choose a save";
            break;
        case MenuScreen::NewSlot:
            result.title = "difficulty";
            result.subtitle = kNewSlotSubtitles[static_cast<size_t>(
                std::clamp(selection_, 0, static_cast<int>(kNewSlotSubtitles.size()) - 1))];
            break;
        case MenuScreen::LevelGroups:
            result.title = "play";
            result.subtitle = chosenSlot_ >= 0 && slots_[static_cast<size_t>(chosenSlot_)].used
                                  ? slots_[static_cast<size_t>(chosenSlot_)].heading.data()
                                  : "choose a set of maps";
            break;
        case MenuScreen::Confirm:
            result.title = confirmTitle_;
            result.subtitle = confirmSubtitle_;
            break;
        case MenuScreen::Pause:
            result.title = "paused";
            result.subtitle = "the mine is waiting";
            break;
        case MenuScreen::Settings:
            result.title = "settings";
            result.subtitle = "choose a group";
            break;
        case MenuScreen::SettingsVideo:
            result.title = "video";
            result.subtitle = "left and right adjust";
            break;
        case MenuScreen::SettingsAudio:
            result.title = "audio";
            result.subtitle = "left and right adjust";
            break;
        case MenuScreen::SettingsGame:
            result.title = "game";
            result.subtitle = "left and right adjust";
            break;
        case MenuScreen::Tutorial:
            result.title = tutorialTitle_;
            result.subtitle = "";
            result.tutorialTitle = tutorialTitle_;
            result.tutorialText = tutorialText_;
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

    const auto [definitions, count] = buttonsFor(screen_, summaryRetry_);
    result.buttonCount = count;
    // The tighter row spacing is for screens that are a column of values to be
    // read against each other. The settings root is a signpost with five plain
    // rows, so it is laid out like the pause menu it came from.
    const bool settingsScreen = editsSettings(screen_);
    const float settingsTop = screenHeight * 0.30f;
    const float settingsBottom = screenHeight * 0.88f;
    const float settingsHeight = settingsBottom - settingsTop;
    const float rowGap =
        settingsScreen ? std::clamp(screenHeight * 0.006f, 2.0f, 10.0f) : gap;
    const float buttonHeight = settingsScreen
                                   ? std::clamp((settingsHeight -
                                                 static_cast<float>(count - 1) * rowGap) /
                                                    static_cast<float>(count),
                                                14.0f, 56.0f)
                                   : std::clamp(screenHeight * 0.065f, 42.0f, 64.0f);
    const float blockHeight = count * buttonHeight + (count - 1) * rowGap;
    const float blockCenter = settingsScreen
                                  ? settingsTop + settingsHeight * 0.5f
                              : screen_ == MenuScreen::Tutorial ? screenHeight * 0.86f
                                                                : screenHeight *
                                        (screen_ == MenuScreen::Summary ? 0.70f : 0.64f);
    const float top = blockCenter - blockHeight * 0.5f;

    for (int i = 0; i < count; ++i) {
        MenuButtonView& button = result.buttons[static_cast<size_t>(i)];
        button.action = definitions[i].action;
        button.label = definitions[i].label;
        button.bounds = {left, top + i * (buttonHeight + rowGap), buttonWidth, buttonHeight};
        button.selected = i == selection_;
        if (screen_ == MenuScreen::Confirm) button.label = i == 0 ? confirmYes_ : confirmNo_;
        if (screen_ == MenuScreen::SaveSlots && i < static_cast<int>(slots_.size())) {
            const SlotSummary& slot = slots_[static_cast<size_t>(i)];
            button.value = slot.value;
            button.detail = slot.detail;
        }
        if (i >= valueRows(screen_)) continue;
        const auto onOff = [&](bool on) {
            std::snprintf(button.value.data(), button.value.size(), "%s", on ? "on" : "off");
        };
        switch (screen_) {
            case MenuScreen::SettingsVideo:
                switch (i) {
                    case 0: onOff(editedSettings_.fullscreen); break;
                    case 1:
                        std::snprintf(button.value.data(), button.value.size(), "%d x %d",
                                      editedSettings_.windowWidth, editedSettings_.windowHeight);
                        break;
                    case 2: onOff(editedSettings_.vsync); break;
                    case 3:
                        std::snprintf(button.value.data(), button.value.size(), "%.0f deg",
                                      editedSettings_.firstPersonFov);
                        break;
                    case 4:
                        std::snprintf(button.value.data(), button.value.size(), "%.0f deg",
                                      editedSettings_.thirdPersonFov);
                        break;
                    default: break;
                }
                break;
            case MenuScreen::SettingsAudio:
                switch (i) {
                    case 0:
                        std::snprintf(
                            button.value.data(), button.value.size(), "%d%%",
                            static_cast<int>(std::round(editedSettings_.masterVolume * 100.0f)));
                        break;
                    case 1:
                        std::snprintf(
                            button.value.data(), button.value.size(), "%d%%",
                            static_cast<int>(std::round(editedSettings_.musicVolume * 100.0f)));
                        break;
                    case 2:
                        std::snprintf(
                            button.value.data(), button.value.size(), "%d%%",
                            static_cast<int>(std::round(editedSettings_.effectsVolume * 100.0f)));
                        break;
                    default: break;
                }
                break;
            case MenuScreen::SettingsGame:
                switch (i) {
                    case 0:
                        std::snprintf(button.value.data(), button.value.size(), "%.1fx",
                                      editedSettings_.mouseSensitivity);
                        break;
                    case 1: onOff(editedSettings_.invertMouseY); break;
                    case 2: onOff(editedSettings_.tutorials); break;
                    case 3: onOff(editedSettings_.weaponHitboxes); break;
                    default: break;
                }
                break;
            default: break;
        }
    }
    return result;
}

}
