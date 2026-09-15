#include "game/settings.h"

#include "core/log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace dvk {

namespace {

constexpr int kSettingsVersion = 3;

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
    settings.firstPersonFov =
        std::clamp(settings.firstPersonFov, kMinVerticalFov, kMaxVerticalFov);
    settings.thirdPersonFov =
        std::clamp(settings.thirdPersonFov, kMinVerticalFov, kMaxVerticalFov);
    settings.windowWidth = std::clamp(settings.windowWidth, 640, 7680);
    settings.windowHeight = std::clamp(settings.windowHeight, 360, 4320);
    settings.bindings = normalizedInputBindings(settings.bindings);
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
        readIf(source, "firstPersonFov", settings.firstPersonFov);
        readIf(source, "thirdPersonFov", settings.thirdPersonFov);
        readIf(source, "invertMouseY", settings.invertMouseY);
        readIf(source, "muted", settings.muted);
        readIf(source, "fullscreen", settings.fullscreen);
        readIf(source, "vsync", settings.vsync);
        readIf(source, "windowWidth", settings.windowWidth);
        readIf(source, "windowHeight", settings.windowHeight);
        const auto bindings = source.find("bindings");
        if (bindings != source.end() && bindings->is_object()) {
            for (size_t i = 0; i < kPlayerActionCount; ++i) {
                const PlayerAction action = static_cast<PlayerAction>(i);
                const auto entry = bindings->find(playerActionName(action));
                if (entry == bindings->end() || !entry->is_object()) continue;
                InputBinding binding = settings.bindings[action];
                std::string device;
                readIf(*entry, "device", device);
                readIf(*entry, "code", binding.code);
                if (device == "keyboard") binding.device = InputDevice::Keyboard;
                else if (device == "mouse") binding.device = InputDevice::Mouse;
                settings.bindings[action] = binding;
            }
        }
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
    nlohmann::json bindings = nlohmann::json::object();
    for (size_t i = 0; i < kPlayerActionCount; ++i) {
        const PlayerAction action = static_cast<PlayerAction>(i);
        const InputBinding binding = settings.bindings[action];
        bindings[playerActionName(action)] = {
            {"device", binding.device == InputDevice::Keyboard ? "keyboard" : "mouse"},
            {"code", binding.code},
        };
    }
    const nlohmann::json output{
        {"version", kSettingsVersion},
        {"masterVolume", settings.masterVolume},
        {"mouseSensitivity", settings.mouseSensitivity},
        {"firstPersonFov", settings.firstPersonFov},
        {"thirdPersonFov", settings.thirdPersonFov},
        {"invertMouseY", settings.invertMouseY},
        {"muted", settings.muted},
        {"fullscreen", settings.fullscreen},
        {"vsync", settings.vsync},
        {"windowWidth", settings.windowWidth},
        {"windowHeight", settings.windowHeight},
        {"bindings", std::move(bindings)},
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

    PlayerSettings outsideLimits;
    outsideLimits.firstPersonFov = 10.0f;
    outsideLimits.thirdPersonFov = 180.0f;
    const PlayerSettings clamped = normalizedPlayerSettings(outsideLimits);
    ok &= expect(clamped.firstPersonFov == kMinVerticalFov &&
                     clamped.thirdPersonFov == kMaxVerticalFov,
                 "field of view stays inside its supported range");

    PlayerSettings written;
    written.masterVolume = 0.35f;
    written.mouseSensitivity = 1.7f;
    written.firstPersonFov = 85.0f;
    written.thirdPersonFov = 95.0f;
    written.invertMouseY = true;
    written.muted = true;
    written.fullscreen = true;
    written.vsync = false;
    written.windowWidth = 1920;
    written.windowHeight = 1080;
    assignInputBinding(written.bindings, PlayerAction::Forward,
                       {InputDevice::Keyboard, 265});
    ok &= expect(savePlayerSettings(path, written), "atomic save succeeds");
    const PlayerSettings loaded = loadPlayerSettings(path);
    ok &= expect(loaded.masterVolume == written.masterVolume &&
                     loaded.mouseSensitivity == written.mouseSensitivity && loaded.invertMouseY &&
                     loaded.firstPersonFov == written.firstPersonFov &&
                     loaded.thirdPersonFov == written.thirdPersonFov &&
                     loaded.muted && loaded.fullscreen && !loaded.vsync &&
                     loaded.windowWidth == 1920 && loaded.windowHeight == 1080,
                 "saved values round trip");
    ok &= expect(loaded.bindings[PlayerAction::Forward] ==
                     InputBinding{InputDevice::Keyboard, 265},
                 "saved bindings round trip");

    {
        std::ofstream partial(path, std::ios::trunc);
        partial << R"({"masterVolume":0.25,"windowWidth":"wide","futureField":42,"bindings":{"forward":{"device":"mouse","code":99},"back":{"device":"keyboard","code":73}}})";
    }
    const PlayerSettings recovered = loadPlayerSettings(path);
    ok &= expect(recovered.masterVolume == 0.25f && recovered.windowWidth == 1600,
                 "partial file keeps valid fields");
    ok &= expect(recovered.bindings[PlayerAction::Forward] ==
                         InputBinding{InputDevice::Keyboard, 87} &&
                     recovered.bindings[PlayerAction::Back] ==
                         InputBinding{InputDevice::Keyboard, 73},
                 "invalid bindings default without discarding valid bindings");

    {
        // A file written before the weapon keys were reorganised: the old
        // weapon1..weapon6 slots named a gun by its position, which no longer
        // exists. None of them may be mistaken for one of the actions that
        // replaced them, so every weapon key comes back as its default.
        std::ofstream previous(path, std::ios::trunc);
        previous << R"({"version":3,"bindings":{"weapon1":{"device":"keyboard","code":49},)"
                    R"("weapon3":{"device":"keyboard","code":90},)"
                    R"("weapon5":{"device":"keyboard","code":88}}})";
    }
    const PlayerSettings migrated = loadPlayerSettings(path);
    const InputBindings fresh = defaultInputBindings();
    bool weaponsDefaulted = true;
    for (const WeaponSelectAction& select : kWeaponSelectActions) {
        weaponsDefaulted =
            weaponsDefaulted && migrated.bindings[select.action] == fresh[select.action];
    }
    ok &= expect(weaponsDefaulted && migrated.bindings[PlayerAction::CycleSecondary] ==
                                         fresh[PlayerAction::CycleSecondary],
                 "weapon bindings from before the reorganisation fall back to the defaults");

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
