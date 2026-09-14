#pragma once

#include <glm/vec2.hpp>

#include <array>

namespace dvk {

enum class MenuScreen {
    Hidden,
    Main,
    Pause,
    Summary,
};

enum class MenuAction {
    None,
    Start,
    Resume,
    Restart,
    MainMenu,
    Quit,
};

struct MenuInput {
    bool previous = false;
    bool next = false;
    bool confirm = false;
    bool pointerPressed = false;
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
};

struct MenuView {
    MenuScreen screen = MenuScreen::Hidden;
    const char* title = "";
    const char* subtitle = "";
    std::array<MenuButtonView, 4> buttons{};
    int buttonCount = 0;
    bool won = false;
    const char* reason = "";
    int elapsedSeconds = 0;
    int robotsDestroyed = 0;
    int robotsTotal = 0;
};

class PlayerMenu {
public:
    void hide();
    void showMain();
    void showPause();
    void showSummary(bool won, const char* reason, float elapsed, int robotsDestroyed,
                     int robotsTotal);

    MenuAction update(const MenuInput& input, int width, int height);
    MenuView view(int width, int height) const;
    MenuScreen screen() const { return screen_; }

private:
    int buttonCount() const;

    MenuScreen screen_ = MenuScreen::Hidden;
    int selection_ = 0;
    bool won_ = false;
    const char* reason_ = "";
    int elapsedSeconds_ = 0;
    int robotsDestroyed_ = 0;
    int robotsTotal_ = 0;
};

}
