#pragma once

#include "core/input.h"
#include "game/menu.h"
#include "sim/ship.h"

#include <GLFW/glfw3.h>

#include <array>

namespace dvk {

// Every key the game reads, in one table. Rebinding later means editing this
// array (or loading it from a config); nothing else in the project names a
// GLFW key code for gameplay.
enum class Action {
    Forward,
    Back,
    StrafeLeft,
    StrafeRight,
    Up,
    Down,
    RollLeft,
    RollRight,
    PitchUp,
    PitchDown,
    YawLeft,
    YawRight,
    Count,
};

struct Binding {
    Action action;
    int key;
    const char* label;
};

inline constexpr std::array<Binding, static_cast<size_t>(Action::Count)> kBindings{{
    {Action::Forward, GLFW_KEY_W, "W"},
    {Action::Back, GLFW_KEY_S, "S"},
    {Action::StrafeLeft, GLFW_KEY_A, "A"},
    {Action::StrafeRight, GLFW_KEY_D, "D"},
    {Action::Up, GLFW_KEY_SPACE, "Space"},
    {Action::Down, GLFW_KEY_LEFT_CONTROL, "Left Ctrl"},
    {Action::RollLeft, GLFW_KEY_Q, "Q"},
    {Action::RollRight, GLFW_KEY_E, "E"},
    {Action::PitchUp, GLFW_KEY_UP, "Up"},
    {Action::PitchDown, GLFW_KEY_DOWN, "Down"},
    {Action::YawLeft, GLFW_KEY_LEFT, "Left"},
    {Action::YawRight, GLFW_KEY_RIGHT, "Right"},
}};

inline int keyFor(Action a) {
    return kBindings[static_cast<size_t>(a)].key;
}

// The same rule for the keys that fire once on press rather than being held.
// These used to be GLFW constants spelled out at their use sites in main.cpp,
// which made CLAUDE.md's "nothing else in the project names a GLFW key code for
// gameplay" false the moment anyone looked.
enum class Toggle {
    Chase,
    DebugUi,
    Headlight,
    Flare,
    Automap,
    Pause,
    Mute,
    Count,
};

struct ToggleBinding {
    Toggle toggle;
    int key;
    const char* label;
};

inline constexpr std::array<ToggleBinding, static_cast<size_t>(Toggle::Count)> kToggles{{
    {Toggle::Chase, GLFW_KEY_F1, "F1"},
    {Toggle::DebugUi, GLFW_KEY_F3, "F3"},
    {Toggle::Headlight, GLFW_KEY_H, "H"},
    {Toggle::Flare, GLFW_KEY_F, "F"},
    {Toggle::Automap, GLFW_KEY_TAB, "Tab"},
    {Toggle::Pause, GLFW_KEY_ESCAPE, "Escape"},
    {Toggle::Mute, GLFW_KEY_M, "M"},
}};

inline int keyFor(Toggle t) {
    return kToggles[static_cast<size_t>(t)].key;
}

// Weapon selection is 1..4 in order, so it is a base rather than a table.
inline constexpr int kWeaponSelectBase = GLFW_KEY_1;

inline MenuInput readMenuInput(const Input& input, const glm::vec2& pointer) {
    MenuInput menu;
    menu.previous = input.pressed(GLFW_KEY_UP) || input.pressed(GLFW_KEY_W);
    menu.next = input.pressed(GLFW_KEY_DOWN) || input.pressed(GLFW_KEY_S);
    menu.decrease = input.pressed(GLFW_KEY_LEFT) || input.pressed(GLFW_KEY_A);
    menu.increase = input.pressed(GLFW_KEY_RIGHT) || input.pressed(GLFW_KEY_D);
    menu.confirm = input.pressed(GLFW_KEY_ENTER) || input.pressed(GLFW_KEY_SPACE);
    menu.pointerPressed = input.mousePressed(GLFW_MOUSE_BUTTON_LEFT);
    menu.pointer = pointer;
    return menu;
}

// Samples the bindings into the device-independent control struct the
// simulation consumes. Mouse motion is only applied while the cursor is
// captured, so alt-tabbing away does not spin the ship.
inline sim::ShipControls readControls(const Input& input, glm::vec2 mouseDelta) {
    sim::ShipControls c;
    const auto axis = [&](Action pos, Action neg) {
        return (input.down(keyFor(pos)) ? 1.0f : 0.0f) -
               (input.down(keyFor(neg)) ? 1.0f : 0.0f);
    };
    c.thrust = axis(Action::Forward, Action::Back);
    c.strafe = axis(Action::StrafeRight, Action::StrafeLeft);
    c.lift = axis(Action::Up, Action::Down);
    c.roll = axis(Action::RollRight, Action::RollLeft);
    c.pitch = axis(Action::PitchUp, Action::PitchDown);
    c.yaw = axis(Action::YawRight, Action::YawLeft);
    if (input.mouseCaptured()) c.look = mouseDelta;
    return c;
}

} // namespace dvk
