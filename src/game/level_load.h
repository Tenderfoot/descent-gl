#pragma once

#include <string>

namespace dvk {

enum class Difficulty : int;
struct Level;
struct ScriptLoadout;
class CommDirector;
class LevelSession;
class ScriptHost;
class TutorialPane;

namespace gl {
class Renderer;
struct AutomapData;
}

namespace sim {
struct ContactLog;
struct RobotConfig;
}

// Everything that is a function of which mine is loaded.
//
// They are named together because loading another mine has to rebuild all of
// them at once: the renderer's level resources, the session's initial and live
// states, and every cache that indexes segments, lights or contacts of the mine
// that was loaded a moment ago. Anything left stale shows up as a light in a
// wall or a ship in the rock, which is why the resets belong in one place
// rather than spread through the menu handlers that trigger them.
//
// References, because these are the application's own objects and this only
// drives them. The three values at the end are the command line's say in what a
// freshly loaded mine starts as.
struct LoadedLevel {
    Level& level;
    std::string& path;
    gl::Renderer& renderer;
    LevelSession& session;
    ScriptHost& script;
    CommDirector& comm;
    TutorialPane& tutorial;
    sim::ContactLog& contacts;
    gl::AutomapData& automap;
    float& automapZoomTarget;
    // The level's own lights had their segment sets computed at load with no
    // doors. -1 is what makes the frame loop recompute them against this
    // level's door state.
    int& lightDoorSignature;
    int& lightEditorSelection;
    const sim::RobotConfig& robots;
    int startSegment;
    bool headlight;
};

// Loads the level's Lua declarations and checks that every clip a message or a
// tutorial names is one the library actually has. Fatal if it is not: a level
// that asks for pictures nobody shipped is a content bug, and finding it at
// load beats finding it when the trigger fires.
void loadLevelScript(ScriptHost& script, const Level& level, const CommDirector& comm);

// Copies what the level's script says the player starts with onto the session.
//
// Onto initial() for the reason the headlight below is: that is the state every
// start and every restart is copied from. The live states are written as well
// so a run that never calls start() - a screenshot, a frame-limited pass -
// still shows the loadout the level asked for.
void applySessionLoadout(LevelSession& session, const ScriptLoadout& loadout);

// Takes the pickups a run at `difficulty` does not have out of the mine, and
// puts back any an earlier difficulty took out. Out means marked collected
// before the run starts: the renderer, the pickup step and the automap all
// already agree that a collected pickup is not there, so nothing else needs
// to learn a second way for one to be missing.
//
// On all three states, for the reason the loadout is.
void applySessionDifficulty(LevelSession& session, const Level& level, Difficulty difficulty);

// The headlight is off at the start, so a frame-limited run has no way to show
// it without someone pressing the key. Set on the initial state rather than the
// current one: start() and every restart copy that over the live state, so
// anything set only on current() lasts until the run actually begins and no
// longer.
void setSessionHeadlight(LevelSession& session, bool on);

// Loading another mine, from the level list and from nowhere else. Does nothing
// when `path` is already what is loaded.
void loadLevelFile(LoadedLevel& loaded, const std::string& path);

} // namespace dvk
