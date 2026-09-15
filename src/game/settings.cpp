#include "game/settings.h"

#include "core/log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace dvk {

namespace {

constexpr int kSettingsVersion = 1;

template <typename T>
void readIf(const nlohmann::json& source, const char* name, T& destination) {
    const auto field = source.find(name);
    if (field != source.end() && field->is_null() == false) {
        try {
            destination = field->get<T>();
        } catch (const nlohmann::json::exception&) {
            log::warn("settings field '{}' has the wrong type; keeping its default", name);
        }
    }
}

bool replaceFile(const std::filesystem::path& source, const std::filesystem::path& destination) {
#ifdef _WIN32
    return MoveFileExW(source.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    return !error;
#endif
}

bool expect(bool condition, const char* message) {
    std::printf("settings-test: %s %s\n", condition ? "PASS" : "FAIL", message);
    return condition;
}

}

PlayerSettings normalizedPlayerSettings(PlayerSettings settings) {
    settings.masterVolume = std::clamp(settings.masterVolume, 0.0f, 1.0f);
    settings.mouseSensitivity = std::clamp(settings.mouseSensitivity, 0.2f, 3.0f);
    settings.windowWidth = std::clamp(settings.windowWidth, 640, 7680);
    settings.windowHeight = std::clamp(settings.windowHeight, 360, 4320);
    return settings;
}

std::filesystem::path defaultPlayerSettingsPath() {
#ifdef _WIN32
    char* localAppData = nullptr;
    size_t length = 0;
    if (_dupenv_s(&localAppData, &length, "LOCALAPPDATA") == 0 && localAppData) {
        const std::filesystem::path result =
            std::filesystem::path(localAppData) / "descent-gl" / "settings.json";
        std::free(localAppData);
        return result;
    }
#else
    if (const char* configHome = std::getenv("XDG_CONFIG_HOME")) {
        return std::filesystem::path(configHome) / "descent-gl" / "settings.json";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".config" / "descent-gl" / "settings.json";
    }
#endif
    return std::filesystem::path("settings.json");
}

PlayerSettings loadPlayerSettings(const std::filesystem::path& path) {
    PlayerSettings settings;
    std::ifstream input(path);
    if (!input) return settings;

    try {
        const nlohmann::json source = nlohmann::json::parse(input);
        readIf(source, "masterVolume", settings.masterVolume);
        readIf(source, "mouseSensitivity", settings.mouseSensitivity);
        readIf(source, "invertMouseY", settings.invertMouseY);
        readIf(source, "muted", settings.muted);
        readIf(source, "fullscreen", settings.fullscreen);
        readIf(source, "vsync", settings.vsync);
        readIf(source, "windowWidth", settings.windowWidth);
        readIf(source, "windowHeight", settings.windowHeight);
    } catch (const nlohmann::json::exception& error) {
        log::warn("could not read settings '{}': {}; using defaults", path.string(), error.what());
        return PlayerSettings{};
    }
    return normalizedPlayerSettings(settings);
}

bool savePlayerSettings(const std::filesystem::path& path, const PlayerSettings& value) {
    const PlayerSettings settings = normalizedPlayerSettings(value);
    std::error_code error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        log::warn("could not create settings directory '{}': {}", path.parent_path().string(),
                  error.message());
        return false;
    }

    const std::filesystem::path temporary = path.string() + ".tmp";
    const nlohmann::json output{
        {"version", kSettingsVersion},
        {"masterVolume", settings.masterVolume},
        {"mouseSensitivity", settings.mouseSensitivity},
        {"invertMouseY", settings.invertMouseY},
        {"muted", settings.muted},
        {"fullscreen", settings.fullscreen},
        {"vsync", settings.vsync},
        {"windowWidth", settings.windowWidth},
        {"windowHeight", settings.windowHeight},
    };
    {
        std::ofstream file(temporary, std::ios::trunc);
        if (!file) {
            log::warn("could not write settings temporary file '{}'", temporary.string());
            return false;
        }
        file << output.dump(2) << '\n';
        file.flush();
        if (!file) {
            log::warn("could not finish writing settings temporary file '{}'", temporary.string());
            return false;
        }
    }
    if (replaceFile(temporary, path)) return true;

    std::filesystem::remove(temporary, error);
    log::warn("could not replace settings file '{}'", path.string());
    return false;
}

int runSettingsTest() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "descent-gl-settings-test";
    const std::filesystem::path path = directory / "settings.json";
    std::error_code error;
    std::filesystem::remove_all(directory, error);

    bool ok = true;
    const PlayerSettings defaults = loadPlayerSettings(path);
    ok &= expect(defaults.masterVolume == 0.8f && defaults.windowWidth == 1600,
                 "missing file uses defaults");

    PlayerSettings written;
    written.masterVolume = 0.35f;
    written.mouseSensitivity = 1.7f;
    written.invertMouseY = true;
    written.muted = true;
    written.fullscreen = true;
    written.vsync = false;
    written.windowWidth = 1920;
    written.windowHeight = 1080;
    ok &= expect(savePlayerSettings(path, written), "atomic save succeeds");
    const PlayerSettings loaded = loadPlayerSettings(path);
    ok &= expect(loaded.masterVolume == written.masterVolume &&
                     loaded.mouseSensitivity == written.mouseSensitivity && loaded.invertMouseY &&
                     loaded.muted && loaded.fullscreen && !loaded.vsync &&
                     loaded.windowWidth == 1920 && loaded.windowHeight == 1080,
                 "saved values round trip");

    {
        std::ofstream partial(path, std::ios::trunc);
        partial << R"({"masterVolume":0.25,"windowWidth":"wide","futureField":42})";
    }
    const PlayerSettings recovered = loadPlayerSettings(path);
    ok &= expect(recovered.masterVolume == 0.25f && recovered.windowWidth == 1600,
                 "partial file keeps valid fields");

    {
        std::ofstream malformed(path, std::ios::trunc);
        malformed << "{";
    }
    const PlayerSettings malformed = loadPlayerSettings(path);
    ok &= expect(malformed.masterVolume == 0.8f && malformed.windowHeight == 900,
                 "malformed file uses defaults");

    std::filesystem::remove_all(directory, error);
    std::printf("settings-test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

}
