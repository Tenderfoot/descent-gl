#pragma once

#include "game/input_actions.h"

#include <filesystem>

namespace dvk {

inline constexpr float kMinVerticalFov = 50.0f;
inline constexpr float kMaxVerticalFov = 120.0f;

struct PlayerSettings {
    float masterVolume = 0.8f;
    float mouseSensitivity = 1.0f;
    float firstPersonFov = 70.0f;
    float thirdPersonFov = 70.0f;
    bool invertMouseY = false;
    bool muted = false;
    bool fullscreen = false;
    bool vsync = true;
    int windowWidth = 1600;
    int windowHeight = 900;
    InputBindings bindings = defaultInputBindings();
};

std::filesystem::path defaultPlayerSettingsPath();
PlayerSettings loadPlayerSettings(const std::filesystem::path& path);
bool savePlayerSettings(const std::filesystem::path& path, const PlayerSettings& settings);
PlayerSettings normalizedPlayerSettings(PlayerSettings settings);
int runSettingsTest();

}
