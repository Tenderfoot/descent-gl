#pragma once

#include <string>
#include <vector>

namespace dvk {

// What the player menu needs to know about a level without loading it: where it
// lives and what to call it. Deliberately not a `Level` - the menu lists every
// level in the directory, and loading and validating all of them to draw a list
// of names would pay for a mine's worth of geometry per entry to show one line
// of text.
struct LevelEntry {
    std::string path;      // as passed to loadLevel()
    std::string name;      // the level's "name" field, or the file stem
    std::string comment;   // the level's "comment" field, may be empty
    int completions = 0;
    int bestSeconds = -1;
};

// Every regular file in `directory` with `extension`, sorted. Shared with the
// asset validator, which walks the same directories for the same reason:
// directory_iterator order is unspecified, and a report - or a menu - whose
// line order changes between machines is one nobody can diff.
std::vector<std::string> filesWithExtension(const char* directory, const char* extension);

// Every level in `directory`, in the order the menu should show them:
// `preferred` first if it is there, then the rest by path. Reads only the
// "name" and "comment" fields of each file; a file that is not readable or not
// JSON is listed under its stem rather than dropped, because a level that is
// present and broken is something the player should see and the loader should
// be the thing that complains about it.
std::vector<LevelEntry> discoverLevels(const char* directory = "assets/levels",
                                       const std::string& preferred = "");

// The levels split the way the player menu offers them: the campaign first, in
// the order `campaignFile` lists it, then every other level in `directory` by
// path. The campaign file names levels by stem, as {"levels": ["TheBeginning",
// ...]}. A campaign level whose file is missing is still listed, under its
// stem, for the same reason a broken one is: the loader says what is wrong.
// A missing or unreadable campaign file is a campaign of nothing, and every
// level in the directory is one of the others.
struct LevelCatalog {
    std::vector<LevelEntry> levels;
    int campaignCount = 0;   // levels[0, campaignCount) is the campaign
};
LevelCatalog discoverCatalog(const char* directory, const char* campaignFile);

}
