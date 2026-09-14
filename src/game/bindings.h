#pragma once

#include "core/input.h"
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
