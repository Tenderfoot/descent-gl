#pragma once

#include "sim/weapons.h"

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
    // One key per primary gun. Named for the gun and not for the number key,
    // so that moving a gun to a different key is a change to one default
    // binding rather than a slot whose meaning silently shifts under every
    // saved settings file. The missiles share one key, which cycles them.
    SelectLaser,
    SelectLightning,
    SelectVulcan,
    SelectRailgun,
    CycleSecondary,
    ChaseCamera,
    Mute,
    // Skips the playing soundtrack on to the next track in its playlist. One
    // key for both sides: which playlist it moves is whichever one the player
    // is currently hearing, and mid-crossfade there is no answer to that, so
    // the key does nothing until the fade lands.
    NextTrack,
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

// Which gun each number key asks for. A written-out pairing rather than an
// index into a weapon list: the actions and the WeaponId enum are two separate
// orders, and a cast between them would quietly select the wrong gun the first
// time either one changed.
struct WeaponSelectAction {
    PlayerAction action;
    sim::WeaponId weapon;
};

inline constexpr std::array<WeaponSelectAction, 4> kWeaponSelectActions{{
    {PlayerAction::SelectLaser, sim::WeaponLaser},
    {PlayerAction::SelectLightning, sim::WeaponLightning},
    {PlayerAction::SelectVulcan, sim::WeaponVulcan},
    {PlayerAction::SelectRailgun, sim::WeaponRailgun},
}};

InputBindings defaultInputBindings();
InputBindings normalizedInputBindings(InputBindings bindings);
void assignInputBinding(InputBindings& bindings, PlayerAction action, InputBinding binding);
const char* playerActionName(PlayerAction action);
const char* playerActionLabel(PlayerAction action);
void formatInputBinding(InputBinding binding, char* destination, size_t destinationSize);

}
