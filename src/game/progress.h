#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace dvk {

struct LevelRecord {
    std::string id;
    int runs = 0;
    int wins = 0;
    float bestTime = -1.0f;
    int mostRobots = 0;
};

struct RunRecordUpdate {
    bool firstCompletion = false;
    bool newBestTime = false;
    bool newRobotRecord = false;
};

class PlayerProgress {
public:
    const LevelRecord* find(const std::string& id) const;
    LevelRecord& findOrAdd(const std::string& id);
    const std::vector<LevelRecord>& levels() const { return levels_; }

private:
    std::vector<LevelRecord> levels_;
};

std::string levelProgressId(const std::string& levelPath);
std::filesystem::path defaultPlayerProgressPath();
PlayerProgress loadPlayerProgress(const std::filesystem::path& path);
bool savePlayerProgress(const std::filesystem::path& path, const PlayerProgress& progress);
RunRecordUpdate recordRun(PlayerProgress& progress, const std::string& levelId, bool won,
                          float elapsed, int robotsDestroyed);
const char* runRecordNote(const RunRecordUpdate& update);
int runProgressTest();

}
