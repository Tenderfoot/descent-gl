#pragma once

#include "core/input.h"
#include "game/input_actions.h"
#include "game/menu.h"
#include "sim/ship.h"

#include <GLFW/glfw3.h>

namespace dvk {

inline bool bindingDown(const Input& input, const InputBindings& bindings, PlayerAction action) {
    const InputBinding binding = bindings[action];
    return binding.device == InputDevice::Keyboard ? input.down(binding.code)
                                                    : input.mouseDown(binding.code);
}

inline bool bindingPressed(const Input& input, const InputBindings& bindings,
                           PlayerAction action) {
    const InputBinding binding = bindings[action];
    return binding.device == InputDevice::Keyboard ? input.pressed(binding.code)
                                                    : input.mousePressed(binding.code);
}

inline MenuInput readMenuInput(const Input& input, const glm::vec2& pointer) {
    MenuInput menu;
    menu.previous = input.pressed(GLFW_KEY_UP) || input.pressed(GLFW_KEY_W);
    menu.next = input.pressed(GLFW_KEY_DOWN) || input.pressed(GLFW_KEY_S);
    menu.decrease = input.pressed(GLFW_KEY_LEFT) || input.pressed(GLFW_KEY_A);
    menu.increase = input.pressed(GLFW_KEY_RIGHT) || input.pressed(GLFW_KEY_D);
    menu.confirm = input.pressed(GLFW_KEY_ENTER) || input.pressed(GLFW_KEY_SPACE);
    menu.pointerPressed = input.mousePressed(GLFW_MOUSE_BUTTON_LEFT);
    menu.cancel = input.pressed(GLFW_KEY_ESCAPE);
    const int key = input.firstPressedKey();
    const int mouseButton = input.firstPressedMouseButton();
    if (key >= 0) {
        menu.bindingPressed = true;
        menu.binding = {InputDevice::Keyboard, key};
    } else if (mouseButton >= 0) {
        menu.bindingPressed = true;
        menu.binding = {InputDevice::Mouse, mouseButton};
    }
    menu.pointer = pointer;
    return menu;
}

// Samples the bindings into the device-independent control struct the
// simulation consumes. Mouse motion is only applied while the cursor is
// captured, so alt-tabbing away does not spin the ship.
inline sim::ShipControls readControls(const Input& input, glm::vec2 mouseDelta,
                                      const InputBindings& bindings) {
    sim::ShipControls c;
    const auto axis = [&](PlayerAction pos, PlayerAction neg) {
        return (bindingDown(input, bindings, pos) ? 1.0f : 0.0f) -
               (bindingDown(input, bindings, neg) ? 1.0f : 0.0f);
    };
    c.thrust = axis(PlayerAction::Forward, PlayerAction::Back);
    c.strafe = axis(PlayerAction::StrafeRight, PlayerAction::StrafeLeft);
    c.lift = axis(PlayerAction::Up, PlayerAction::Down);
    c.roll = axis(PlayerAction::RollRight, PlayerAction::RollLeft);
    c.pitch = axis(PlayerAction::PitchUp, PlayerAction::PitchDown);
    c.yaw = axis(PlayerAction::YawRight, PlayerAction::YawLeft);
    if (input.mouseCaptured()) c.look = mouseDelta;
    return c;
}

} // namespace dvk
