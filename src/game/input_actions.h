#pragma once

#include <array>
#include <cstddef>

namespace dvk {

enum class InputDevice {
    Keyboard,
    Mouse,
};

struct InputBinding {
    InputDevice device = InputDevice::Keyboard;
    int code = 0;

    bool operator==(const InputBinding&) const = default;
};

enum class PlayerAction {
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
    PrimaryFire,
    SecondaryFire,
    Flare,
    Headlight,
    Automap,
    Weapon1,
    Weapon2,
    Weapon3,
    Weapon4,
    Weapon5,
    Weapon6,
    ChaseCamera,
    Mute,
    Pause,
    Count,
};

inline constexpr size_t kPlayerActionCount = static_cast<size_t>(PlayerAction::Count);

struct InputBindings {
    std::array<InputBinding, kPlayerActionCount> values{};

    const InputBinding& operator[](PlayerAction action) const {
        return values[static_cast<size_t>(action)];
    }
    InputBinding& operator[](PlayerAction action) {
        return values[static_cast<size_t>(action)];
    }
};

InputBindings defaultInputBindings();
InputBindings normalizedInputBindings(InputBindings bindings);
void assignInputBinding(InputBindings& bindings, PlayerAction action, InputBinding binding);
const char* playerActionName(PlayerAction action);
const char* playerActionLabel(PlayerAction action);
void formatInputBinding(InputBinding binding, char* destination, size_t destinationSize);

}
