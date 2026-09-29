#pragma once

#include "game/input_actions.h"

#include <filesystem>

namespace dvk {

inline constexpr float kMinVerticalFov = 50.0f;
inline constexpr float kMaxVerticalFov = 120.0f;

struct PlayerSettings {
    float masterVolume = 0.8f;
    float musicVolume = 0.45f;
    float effectsVolume = 1.0f;
    float mouseSensitivity = 1.0f;
    float firstPersonFov = 70.0f;
    float thirdPersonFov = 70.0f;
    bool invertMouseY = false;
    bool muted = false;
    // Whether a level's scripted tutorial panes are allowed to stop the game.
    // On by default: a player who has not turned them off has not seen them.
    bool tutorials = true;
    // Outline the hitbox of each weapon still mounted on a robot, so the
    // player can see what to aim at to shoot it off.
    bool weaponHitboxes = true;
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
