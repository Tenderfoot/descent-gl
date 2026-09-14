#include "game/level_list.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace dvk {

std::vector<std::string> filesWithExtension(const char* directory, const char* extension) {
    std::vector<std::string> found;
    std::error_code ec;
    const std::filesystem::path dir(directory);
    if (!std::filesystem::is_directory(dir, ec)) return found;

    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() != extension) continue;
        found.push_back(entry.path().generic_string());
    }
    std::sort(found.begin(), found.end());
    return found;
}

std::vector<LevelEntry> discoverLevels(const char* directory, const std::string& preferred) {
    std::vector<LevelEntry> levels;
    for (const std::string& path : filesWithExtension(directory, ".json")) {
        LevelEntry entry;
        entry.path = path;
        entry.name = std::filesystem::path(path).stem().generic_string();

        // Only the two header fields. The menu does not need the mine, and an
        // unparsable file is not fatal here: loadLevel says why, loudly, if the
        // player picks it.
        std::ifstream in(path);
        if (in) {
            nlohmann::json j;
            try {
                in >> j;
                entry.name = j.value("name", entry.name);
                entry.comment = j.value("comment", std::string());
            } catch (const std::exception&) {
                // Leave the stem as the name; the loader reports the parse error.
            }
        }
        levels.push_back(std::move(entry));
    }

    // The preferred level is whatever a plain run would have started, so it is
    // the one under the cursor when the list opens.
    const auto it = std::find_if(levels.begin(), levels.end(),
                                 [&](const LevelEntry& e) { return e.path == preferred; });
    if (it != levels.end()) std::rotate(levels.begin(), it, it + 1);
    return levels;
}

}
