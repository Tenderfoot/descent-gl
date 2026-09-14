#include "game/menu.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace dvk {

namespace {

struct ButtonDefinition {
    MenuAction action;
    const char* label;
};

constexpr std::array<ButtonDefinition, 2> kMainButtons{{
    {MenuAction::Start, "play"},
    {MenuAction::Quit, "quit"},
}};

constexpr std::array<ButtonDefinition, 4> kPauseButtons{{
    {MenuAction::Resume, "resume"},
    {MenuAction::Restart, "restart level"},
    {MenuAction::MainMenu, "main menu"},
    {MenuAction::Quit, "quit"},
}};

constexpr std::array<ButtonDefinition, 2> kSummaryButtons{{
    {MenuAction::Restart, "retry"},
    {MenuAction::MainMenu, "main menu"},
}};

std::pair<const ButtonDefinition*, int> buttonsFor(MenuScreen screen) {
    switch (screen) {
        case MenuScreen::Main: return {kMainButtons.data(), static_cast<int>(kMainButtons.size())};
        case MenuScreen::Pause:
            return {kPauseButtons.data(), static_cast<int>(kPauseButtons.size())};
        case MenuScreen::Summary:
            return {kSummaryButtons.data(), static_cast<int>(kSummaryButtons.size())};
        case MenuScreen::Hidden: return {nullptr, 0};
    }
    return {nullptr, 0};
}

}

void PlayerMenu::hide() {
    screen_ = MenuScreen::Hidden;
    selection_ = 0;
}

void PlayerMenu::showMain() {
    screen_ = MenuScreen::Main;
    selection_ = 0;
}

void PlayerMenu::showPause() {
    screen_ = MenuScreen::Pause;
    selection_ = 0;
}

void PlayerMenu::showSummary(bool won, const char* reason, float elapsed, int robotsDestroyed,
                             int robotsTotal) {
    screen_ = MenuScreen::Summary;
    selection_ = 0;
    won_ = won;
    reason_ = reason;
    elapsedSeconds_ = std::max(0, static_cast<int>(elapsed));
    robotsDestroyed_ = robotsDestroyed;
    robotsTotal_ = robotsTotal;
}

int PlayerMenu::buttonCount() const {
    return buttonsFor(screen_).second;
}

MenuAction PlayerMenu::update(const MenuInput& input, int width, int height) {
    const int count = buttonCount();
    if (count == 0) return MenuAction::None;

    const MenuView current = view(width, height);
    for (int i = 0; i < current.buttonCount; ++i) {
        if (current.buttons[static_cast<size_t>(i)].bounds.contains(input.pointer)) selection_ = i;
    }

    if (input.previous) selection_ = (selection_ + count - 1) % count;
    if (input.next) selection_ = (selection_ + 1) % count;

    const MenuView updated = view(width, height);
    if (input.pointerPressed) {
        for (int i = 0; i < updated.buttonCount; ++i) {
            const MenuButtonView& button = updated.buttons[static_cast<size_t>(i)];
            if (button.bounds.contains(input.pointer)) return button.action;
        }
    }
    if (input.confirm) return updated.buttons[static_cast<size_t>(selection_)].action;
    return MenuAction::None;
}

MenuView PlayerMenu::view(int width, int height) const {
    MenuView result;
    result.screen = screen_;
    result.won = won_;
    result.reason = reason_;
    result.elapsedSeconds = elapsedSeconds_;
    result.robotsDestroyed = robotsDestroyed_;
    result.robotsTotal = robotsTotal_;

    switch (screen_) {
        case MenuScreen::Main:
            result.title = "descent";
            result.subtitle = "into the dark";
            break;
        case MenuScreen::Pause:
            result.title = "paused";
            result.subtitle = "the mine is waiting";
            break;
        case MenuScreen::Summary:
            result.title = won_ ? "mine destroyed" : "you are dead";
            result.subtitle = won_ ? "run complete" : "run ended";
            break;
        case MenuScreen::Hidden: return result;
    }

    const auto [definitions, count] = buttonsFor(screen_);
    result.buttonCount = count;
    const float screenWidth = static_cast<float>(std::max(width, 1));
    const float screenHeight = static_cast<float>(std::max(height, 1));
    const float buttonWidth = std::clamp(screenWidth * 0.32f, 260.0f, 460.0f);
    const float buttonHeight = std::clamp(screenHeight * 0.065f, 42.0f, 64.0f);
    const float gap = std::clamp(screenHeight * 0.018f, 10.0f, 18.0f);
    const float blockHeight = count * buttonHeight + (count - 1) * gap;
    const float blockCenter = screenHeight * (screen_ == MenuScreen::Summary ? 0.70f : 0.64f);
    const float top = blockCenter - blockHeight * 0.5f;
    const float left = (screenWidth - buttonWidth) * 0.5f;

    for (int i = 0; i < count; ++i) {
        MenuButtonView& button = result.buttons[static_cast<size_t>(i)];
        button.action = definitions[i].action;
        button.label = definitions[i].label;
        button.bounds = {left, top + i * (buttonHeight + gap), buttonWidth, buttonHeight};
        button.selected = i == selection_;
    }
    return result;
}

}
