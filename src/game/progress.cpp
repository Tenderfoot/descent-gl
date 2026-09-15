#include "game/progress.h"

#include "core/log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace dvk {

namespace {

constexpr int kProgressVersion = 1;

template <typename T>
void readIf(const nlohmann::json& source, const char* name, T& destination) {
    const auto field = source.find(name);
    if (field == source.end() || field->is_null()) return;
    try {
        destination = field->get<T>();
    } catch (const nlohmann::json::exception&) {
        log::warn("progress field '{}' has the wrong type; keeping its default", name);
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
    std::printf("progress-test: %s %s\n", condition ? "PASS" : "FAIL", message);
    return condition;
}

}

const LevelRecord* PlayerProgress::find(const std::string& id) const {
    const auto found = std::find_if(levels_.begin(), levels_.end(),
                                    [&](const LevelRecord& record) { return record.id == id; });
    return found == levels_.end() ? nullptr : &*found;
}

LevelRecord& PlayerProgress::findOrAdd(const std::string& id) {
    const auto found = std::find_if(levels_.begin(), levels_.end(),
                                    [&](const LevelRecord& record) { return record.id == id; });
    if (found != levels_.end()) return *found;
    levels_.push_back(LevelRecord{.id = id});
    return levels_.back();
}

std::string levelProgressId(const std::string& levelPath) {
    return std::filesystem::path(levelPath).filename().generic_string();
}

std::filesystem::path defaultPlayerProgressPath() {
#ifdef _WIN32
    char* localAppData = nullptr;
    size_t length = 0;
    if (_dupenv_s(&localAppData, &length, "LOCALAPPDATA") == 0 && localAppData) {
        const std::filesystem::path result =
            std::filesystem::path(localAppData) / "descent-gl" / "progress.json";
        std::free(localAppData);
        return result;
    }
#else
    if (const char* configHome = std::getenv("XDG_CONFIG_HOME")) {
        return std::filesystem::path(configHome) / "descent-gl" / "progress.json";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".config" / "descent-gl" / "progress.json";
    }
#endif
    return std::filesystem::path("progress.json");
}

PlayerProgress loadPlayerProgress(const std::filesystem::path& path) {
    PlayerProgress progress;
    std::ifstream input(path);
    if (!input) return progress;
    try {
        const nlohmann::json source = nlohmann::json::parse(input);
        const auto levels = source.find("levels");
        if (levels == source.end() || !levels->is_object()) return progress;
        for (auto entry = levels->begin(); entry != levels->end(); ++entry) {
            if (!entry.value().is_object()) continue;
            LevelRecord& record = progress.findOrAdd(entry.key());
            readIf(entry.value(), "runs", record.runs);
            readIf(entry.value(), "wins", record.wins);
            readIf(entry.value(), "bestTime", record.bestTime);
            readIf(entry.value(), "mostRobots", record.mostRobots);
            record.runs = std::max(record.runs, 0);
            record.wins = std::clamp(record.wins, 0, record.runs);
            record.bestTime = record.bestTime > 0.0f ? record.bestTime : -1.0f;
            record.mostRobots = std::max(record.mostRobots, 0);
        }
    } catch (const nlohmann::json::exception& error) {
        log::warn("could not read progress '{}': {}; using empty progress", path.string(),
                  error.what());
        return PlayerProgress{};
    }
    return progress;
}

bool savePlayerProgress(const std::filesystem::path& path, const PlayerProgress& progress) {
    std::error_code error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        log::warn("could not create progress directory '{}': {}", path.parent_path().string(),
                  error.message());
        return false;
    }

    nlohmann::json levels = nlohmann::json::object();
    for (const LevelRecord& record : progress.levels()) {
        levels[record.id] = {
            {"runs", record.runs},
            {"wins", record.wins},
            {"bestTime", record.bestTime},
            {"mostRobots", record.mostRobots},
        };
    }
    const nlohmann::json output{{"version", kProgressVersion}, {"levels", std::move(levels)}};
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::trunc);
        if (!file) {
            log::warn("could not write progress temporary file '{}'", temporary.string());
            return false;
        }
        file << output.dump(2) << '\n';
        file.flush();
        if (!file) {
            log::warn("could not finish writing progress temporary file '{}'", temporary.string());
            return false;
        }
    }
    if (replaceFile(temporary, path)) return true;
    std::filesystem::remove(temporary, error);
    log::warn("could not replace progress file '{}'", path.string());
    return false;
}

RunRecordUpdate recordRun(PlayerProgress& progress, const std::string& levelId, bool won,
                          float elapsed, int robotsDestroyed) {
    LevelRecord& record = progress.findOrAdd(levelId);
    RunRecordUpdate update;
    ++record.runs;
    if (won) {
        update.firstCompletion = record.wins == 0;
        ++record.wins;
        update.newBestTime = elapsed > 0.0f && (record.bestTime < 0.0f || elapsed < record.bestTime);
        if (update.newBestTime) record.bestTime = elapsed;
    }
    update.newRobotRecord = robotsDestroyed > record.mostRobots;
    record.mostRobots = std::max(record.mostRobots, robotsDestroyed);
    return update;
}

const char* runRecordNote(const RunRecordUpdate& update) {
    if (update.firstCompletion) return "first completion";
    if (update.newBestTime && update.newRobotRecord) return "new best time and robot record";
    if (update.newBestTime) return "new best time";
    if (update.newRobotRecord) return "new robot record";
    return "";
}

int runProgressTest() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "descent-gl-progress-test";
    const std::filesystem::path path = directory / "progress.json";
    std::error_code error;
    std::filesystem::remove_all(directory, error);

    bool ok = true;
    PlayerProgress progress = loadPlayerProgress(path);
    ok &= expect(progress.levels().empty(), "missing file starts empty");
    const RunRecordUpdate first = recordRun(progress, "mine01.json", true, 90.0f, 8);
    ok &= expect(first.firstCompletion && first.newBestTime && first.newRobotRecord,
                 "first win establishes every record");
    const RunRecordUpdate slower = recordRun(progress, "mine01.json", true, 120.0f, 5);
    ok &= expect(!slower.firstCompletion && !slower.newBestTime && !slower.newRobotRecord,
                 "a slower run leaves records intact");
    const RunRecordUpdate loss = recordRun(progress, "mine01.json", false, 30.0f, 12);
    ok &= expect(!loss.newBestTime && loss.newRobotRecord,
                 "a loss can set a robot record but not a completion time");
    ok &= expect(savePlayerProgress(path, progress), "atomic save succeeds");
    const PlayerProgress loaded = loadPlayerProgress(path);
    const LevelRecord* record = loaded.find("mine01.json");
    ok &= expect(record && record->runs == 3 && record->wins == 2 && record->bestTime == 90.0f &&
                     record->mostRobots == 12,
                 "saved progress round trips");
    ok &= expect(levelProgressId("assets/levels/mine01.json") == "mine01.json",
                 "level identity ignores display text and directories");

    {
        std::ofstream partial(path, std::ios::trunc);
        partial << R"({"levels":{"mine01.json":{"runs":4,"wins":"many","bestTime":75.0},"future.json":{"runs":1,"wins":1,"futureField":true}}})";
    }
    const PlayerProgress partial = loadPlayerProgress(path);
    const LevelRecord* partialRecord = partial.find("mine01.json");
    ok &= expect(partialRecord && partialRecord->runs == 4 && partialRecord->wins == 0 &&
                     partialRecord->bestTime == 75.0f && partial.find("future.json"),
                 "partial and newer progress keeps every valid field");

    {
        std::ofstream malformed(path, std::ios::trunc);
        malformed << "{";
    }
    ok &= expect(loadPlayerProgress(path).levels().empty(), "malformed progress starts empty");

    std::filesystem::remove_all(directory, error);
    std::printf("progress-test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

}
