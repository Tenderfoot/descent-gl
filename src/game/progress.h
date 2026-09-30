#pragma once

#include "game/level.h"

#include <array>
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

inline constexpr int kSaveSlotCount = 3;

// One player's game: the difficulty it was started at, which never changes
// afterwards, and the records it has built up.
//
// How far the campaign has got is not stored apart from the records. It is the
// campaign levels this slot has won, read against the campaign's order, so a
// reordered campaign.json cannot leave a slot pointing at the wrong level.
struct SaveSlot {
    bool used = false;
    Difficulty difficulty = Difficulty::Normal;
    // Hard, on a slot that a failed run deletes: dying, not getting out before
    // the mine goes, or leaving a level part way through.
    bool ultraHard = false;
    PlayerProgress progress;
};

struct SaveSlots {
    std::array<SaveSlot, kSaveSlotCount> slots{};
};

// "easy", "normal", "hard" or "ultra-hard". What the menu shows and what the
// file stores.
const char* saveSlotDifficultyName(const SaveSlot& slot);

std::string levelProgressId(const std::string& levelPath);
std::filesystem::path defaultPlayerProgressPath();
// A file from before there were slots is one player's records with no
// difficulty. It loads as slot 1 at normal, and is written back as slots the
// next time anything is saved.
SaveSlots loadSaveSlots(const std::filesystem::path& path);
bool saveSaveSlots(const std::filesystem::path& path, const SaveSlots& saves);
RunRecordUpdate recordRun(PlayerProgress& progress, const std::string& levelId, bool won,
                          float elapsed, int robotsDestroyed);
const char* runRecordNote(const RunRecordUpdate& update);
int runProgressTest();

}
