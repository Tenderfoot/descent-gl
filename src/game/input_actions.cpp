#include "game/input_actions.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cstdio>

namespace dvk {

namespace {

struct ActionDefinition {
    const char* name;
    const char* label;
    InputBinding binding;
};

constexpr InputBinding key(int code) {
    return {InputDevice::Keyboard, code};
}

constexpr InputBinding mouse(int code) {
    return {InputDevice::Mouse, code};
}

constexpr std::array<ActionDefinition, kPlayerActionCount> kActions{{
    {"forward", "forward", key(GLFW_KEY_W)},
    {"back", "back", key(GLFW_KEY_S)},
    {"strafeLeft", "strafe left", key(GLFW_KEY_A)},
    {"strafeRight", "strafe right", key(GLFW_KEY_D)},
    {"up", "move up", key(GLFW_KEY_SPACE)},
    {"down", "move down", key(GLFW_KEY_LEFT_CONTROL)},
    {"rollLeft", "roll left", key(GLFW_KEY_Q)},
    {"rollRight", "roll right", key(GLFW_KEY_E)},
    {"pitchUp", "pitch up", key(GLFW_KEY_UP)},
    {"pitchDown", "pitch down", key(GLFW_KEY_DOWN)},
    {"yawLeft", "yaw left", key(GLFW_KEY_LEFT)},
    {"yawRight", "yaw right", key(GLFW_KEY_RIGHT)},
    {"primaryFire", "primary fire", mouse(GLFW_MOUSE_BUTTON_LEFT)},
    {"secondaryFire", "secondary fire", mouse(GLFW_MOUSE_BUTTON_RIGHT)},
    {"flare", "flare", key(GLFW_KEY_F)},
    {"headlight", "headlight", key(GLFW_KEY_H)},
    {"automap", "automap", key(GLFW_KEY_TAB)},
    {"selectLaser", "select laser", key(GLFW_KEY_1)},
    {"selectLightning", "select static charge", key(GLFW_KEY_2)},
    {"selectVulcan", "select vulcan", key(GLFW_KEY_3)},
    {"selectRailgun", "select railgun", key(GLFW_KEY_4)},
    {"cycleSecondary", "cycle missile", key(GLFW_KEY_G)},
    {"chaseCamera", "chase camera", key(GLFW_KEY_F1)},
    {"mute", "mute", key(GLFW_KEY_M)},
    {"nextTrack", "next track", key(GLFW_KEY_N)},
    {"pause", "pause", key(GLFW_KEY_ESCAPE)},
}};

bool valid(InputBinding binding) {
    if (binding.device == InputDevice::Keyboard) {
        return binding.code >= GLFW_KEY_SPACE && binding.code <= GLFW_KEY_LAST;
    }
    return binding.code >= GLFW_MOUSE_BUTTON_1 && binding.code <= GLFW_MOUSE_BUTTON_LAST;
}

}

InputBindings defaultInputBindings() {
    InputBindings bindings;
    for (size_t i = 0; i < kActions.size(); ++i) bindings.values[i] = kActions[i].binding;
    return bindings;
}

InputBindings normalizedInputBindings(InputBindings bindings) {
    const InputBindings defaults = defaultInputBindings();
    for (size_t i = 0; i < bindings.values.size(); ++i) {
        if (!valid(bindings.values[i])) bindings.values[i] = defaults.values[i];
    }
    return bindings;
}

void assignInputBinding(InputBindings& bindings, PlayerAction action, InputBinding binding) {
    if (!valid(binding)) return;
    const size_t selected = static_cast<size_t>(action);
    const InputBinding previous = bindings.values[selected];
    for (size_t i = 0; i < bindings.values.size(); ++i) {
        if (i != selected && bindings.values[i] == binding) {
            bindings.values[i] = previous;
            break;
        }
    }
    bindings.values[selected] = binding;
}

const char* playerActionName(PlayerAction action) {
    return kActions[static_cast<size_t>(action)].name;
}

const char* playerActionLabel(PlayerAction action) {
    return kActions[static_cast<size_t>(action)].label;
}

void formatInputBinding(InputBinding binding, char* destination, size_t destinationSize) {
    if (destinationSize == 0) return;
    if (binding.device == InputDevice::Mouse) {
        constexpr std::array<const char*, 8> names{{
            "mouse left", "mouse right", "mouse middle", "mouse 4",
            "mouse 5", "mouse 6", "mouse 7", "mouse 8",
        }};
        const int index = std::clamp(binding.code, 0, static_cast<int>(names.size()) - 1);
        std::snprintf(destination, destinationSize, "%s", names[static_cast<size_t>(index)]);
        return;
    }
    if ((binding.code >= GLFW_KEY_0 && binding.code <= GLFW_KEY_9) ||
        (binding.code >= GLFW_KEY_A && binding.code <= GLFW_KEY_Z)) {
        std::snprintf(destination, destinationSize, "%c", binding.code);
        return;
    }
    const char* name = nullptr;
    switch (binding.code) {
        case GLFW_KEY_SPACE: name = "space"; break;
        case GLFW_KEY_APOSTROPHE: name = "apostrophe"; break;
        case GLFW_KEY_COMMA: name = "comma"; break;
        case GLFW_KEY_MINUS: name = "minus"; break;
        case GLFW_KEY_PERIOD: name = "period"; break;
        case GLFW_KEY_SLASH: name = "slash"; break;
        case GLFW_KEY_SEMICOLON: name = "semicolon"; break;
        case GLFW_KEY_EQUAL: name = "equal"; break;
        case GLFW_KEY_LEFT_BRACKET: name = "left bracket"; break;
        case GLFW_KEY_BACKSLASH: name = "backslash"; break;
        case GLFW_KEY_RIGHT_BRACKET: name = "right bracket"; break;
        case GLFW_KEY_GRAVE_ACCENT: name = "grave"; break;
        case GLFW_KEY_ESCAPE: name = "escape"; break;
        case GLFW_KEY_ENTER: name = "enter"; break;
        case GLFW_KEY_TAB: name = "tab"; break;
        case GLFW_KEY_BACKSPACE: name = "backspace"; break;
        case GLFW_KEY_INSERT: name = "insert"; break;
        case GLFW_KEY_DELETE: name = "delete"; break;
        case GLFW_KEY_RIGHT: name = "right"; break;
        case GLFW_KEY_LEFT: name = "left"; break;
        case GLFW_KEY_DOWN: name = "down"; break;
        case GLFW_KEY_UP: name = "up"; break;
        case GLFW_KEY_PAGE_UP: name = "page up"; break;
        case GLFW_KEY_PAGE_DOWN: name = "page down"; break;
        case GLFW_KEY_HOME: name = "home"; break;
        case GLFW_KEY_END: name = "end"; break;
        case GLFW_KEY_CAPS_LOCK: name = "caps lock"; break;
        case GLFW_KEY_SCROLL_LOCK: name = "scroll lock"; break;
        case GLFW_KEY_NUM_LOCK: name = "num lock"; break;
        case GLFW_KEY_PRINT_SCREEN: name = "print screen"; break;
        case GLFW_KEY_PAUSE: name = "pause"; break;
        case GLFW_KEY_F1: name = "f1"; break;
        case GLFW_KEY_F2: name = "f2"; break;
        case GLFW_KEY_F3: name = "f3"; break;
        case GLFW_KEY_F4: name = "f4"; break;
        case GLFW_KEY_F5: name = "f5"; break;
        case GLFW_KEY_F6: name = "f6"; break;
        case GLFW_KEY_F7: name = "f7"; break;
        case GLFW_KEY_F8: name = "f8"; break;
        case GLFW_KEY_F9: name = "f9"; break;
        case GLFW_KEY_F10: name = "f10"; break;
        case GLFW_KEY_F11: name = "f11"; break;
        case GLFW_KEY_F12: name = "f12"; break;
        case GLFW_KEY_F13: name = "f13"; break;
        case GLFW_KEY_F14: name = "f14"; break;
        case GLFW_KEY_F15: name = "f15"; break;
        case GLFW_KEY_F16: name = "f16"; break;
        case GLFW_KEY_F17: name = "f17"; break;
        case GLFW_KEY_F18: name = "f18"; break;
        case GLFW_KEY_F19: name = "f19"; break;
        case GLFW_KEY_F20: name = "f20"; break;
        case GLFW_KEY_F21: name = "f21"; break;
        case GLFW_KEY_F22: name = "f22"; break;
        case GLFW_KEY_F23: name = "f23"; break;
        case GLFW_KEY_F24: name = "f24"; break;
        case GLFW_KEY_F25: name = "f25"; break;
        case GLFW_KEY_KP_0: name = "keypad 0"; break;
        case GLFW_KEY_KP_1: name = "keypad 1"; break;
        case GLFW_KEY_KP_2: name = "keypad 2"; break;
        case GLFW_KEY_KP_3: name = "keypad 3"; break;
        case GLFW_KEY_KP_4: name = "keypad 4"; break;
        case GLFW_KEY_KP_5: name = "keypad 5"; break;
        case GLFW_KEY_KP_6: name = "keypad 6"; break;
        case GLFW_KEY_KP_7: name = "keypad 7"; break;
        case GLFW_KEY_KP_8: name = "keypad 8"; break;
        case GLFW_KEY_KP_9: name = "keypad 9"; break;
        case GLFW_KEY_KP_DECIMAL: name = "keypad decimal"; break;
        case GLFW_KEY_KP_DIVIDE: name = "keypad divide"; break;
        case GLFW_KEY_KP_MULTIPLY: name = "keypad multiply"; break;
        case GLFW_KEY_KP_SUBTRACT: name = "keypad subtract"; break;
        case GLFW_KEY_KP_ADD: name = "keypad add"; break;
        case GLFW_KEY_KP_ENTER: name = "keypad enter"; break;
        case GLFW_KEY_KP_EQUAL: name = "keypad equal"; break;
        case GLFW_KEY_LEFT_SHIFT: name = "left shift"; break;
        case GLFW_KEY_LEFT_CONTROL: name = "left ctrl"; break;
        case GLFW_KEY_LEFT_ALT: name = "left alt"; break;
        case GLFW_KEY_RIGHT_SHIFT: name = "right shift"; break;
        case GLFW_KEY_RIGHT_CONTROL: name = "right ctrl"; break;
        case GLFW_KEY_RIGHT_ALT: name = "right alt"; break;
        case GLFW_KEY_LEFT_SUPER: name = "left super"; break;
        case GLFW_KEY_RIGHT_SUPER: name = "right super"; break;
        case GLFW_KEY_MENU: name = "menu"; break;
        default: break;
    }
    if (name) std::snprintf(destination, destinationSize, "%s", name);
    else std::snprintf(destination, destinationSize, "key %d", binding.code);
}

}
