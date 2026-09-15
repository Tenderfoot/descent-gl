#pragma once

#include <filesystem>

namespace dvk {

struct PlayerSettings {
    float masterVolume = 0.8f;
    float mouseSensitivity = 1.0f;
    bool invertMouseY = false;
    bool muted = false;
    bool fullscreen = false;
    bool vsync = true;
    int windowWidth = 1600;
    int windowHeight = 900;
};

std::filesystem::path defaultPlayerSettingsPath();
PlayerSettings loadPlayerSettings(const std::filesystem::path& path);
bool savePlayerSettings(const std::filesystem::path& path, const PlayerSettings& settings);
PlayerSettings normalizedPlayerSettings(PlayerSettings settings);
int runSettingsTest();

}
