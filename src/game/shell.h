#pragma once

#include "game/menu.h"
#include "game/progress.h"
#include "game/settings.h"

#include <filesystem>

namespace dvk {

class Input;
class LevelSession;
class Window;
struct LoadedLevel;
struct ScriptTutorial;

namespace audio {
class Audio;
}

namespace sim {
struct GameState;
}

// How long an unconfirmed display change stands before it is put back.
inline constexpr double kDisplayConfirmationSeconds = 15.0;

// What the frame loop has to do about something the shell just did.
//
// Returned rather than reached out and done, because both of these belong to
// the loop and not to the shell: the accumulator is the fixed step's, and the
// previous state is what rendering interpolates from.
struct ShellEffect {
    // The simulation was stopped, started, or stood still behind a menu. Time
    // banked before that is not time the simulation owes anyone, and paying it
    // back would be a burst of catch-up steps the moment play resumes.
    bool resetAccumulator = false;
    // Collapse the interpolation onto the current state. Whatever happens next
    // should be drawn where it is rather than blended out of wherever the ship
    // was before the menu went up.
    bool resyncPrevious = false;
};

// The player's shell: the menus, the settings they change, the save slots
// their run history goes into, and the countdown that puts a display change
// back if nobody confirms it.
//
// It owns the menu, the settings and the save slots, because nothing outside
// it writes them. It drives the window, the input, the session and the loaded
// level, which belong to the application, so those are held by reference and
// must outlive it.
class Shell {
public:
    // `vsyncAllowed` is false on a frame-limited run, which must not be pinned
    // to the refresh rate whatever the player's setting says. It is decided
    // once from the command line rather than read here, so the shell needs no
    // opinion about why.
    Shell(Window& window, Input& input, audio::Audio& audio, LevelSession& session,
          LoadedLevel& loaded, PlayerSettings settings, std::filesystem::path settingsPath,
          SaveSlots saves, std::filesystem::path savesPath, bool vsyncAllowed);

    PlayerMenu& menu() { return menu_; }
    const PlayerMenu& menu() const { return menu_; }
    const PlayerSettings& settings() const { return settings_; }
    const SaveSlots& saves() const { return saves_; }

    // Rewrites what the slot screen says about each slot. Needs the menu's
    // level list, which is how a slot's campaign is counted, so it is called
    // once that has been set and again whenever a slot changes.
    void refreshSlotSummaries();

    // Pushes the settings at the window and the mixer. The revert goes through
    // here too, so a rejected display change is undone by the path that applied
    // it rather than by a second copy of it.
    void applyRuntimeSettings();

    // The mixer's half of the above, on its own. Start-up wants exactly this:
    // the window was created from these settings already, and asking it to
    // resize itself to the size it is would move a window the window manager
    // has just placed.
    void applyAudioSettings();

    void captureMouse(bool captured);

    // True while the controls screen is waiting for a key. The pause and escape
    // keys belong to that screen while it is, or rebinding either of them would
    // be impossible.
    bool capturingBinding() const;

    // The one thing in here that runs on the clock.
    void tickDisplayConfirmation(double now);

    // Escape, and whatever the player bound pause to. Backs out of the nested
    // screens one at a time before it reaches the pause menu itself, applying
    // the edits on the way out of any screen that has some - leaving a
    // settings screen commits it, whichever way you leave.
    //
    // `now` is the application clock, needed because backing out of the video
    // screen can start the display-change countdown.
    ShellEffect handlePauseKeys(bool pausePressed, bool escapePressed, double now);

    // Whatever the menu decided this frame.
    ShellEffect applyMenuAction(MenuAction action, double now);

    // A level's script asked for a tutorial pane. Stops the simulation, hands
    // the cursor back and puts the pane up; does nothing if the player has
    // turned tutorials off, or if there is no run to stop.
    //
    // Whether the run is one a tutorial belongs in at all is the frame loop's
    // question rather than this one's - a screenshot pass has a perfectly
    // valid session and still must not be stopped by one - so that guard lives
    // with the caller and this one only knows about the setting.
    ShellEffect showTutorial(const ScriptTutorial& tutorial, double now);

    // Mute keeps the device open and the ambient channels running, so unmuting
    // is instant and does not restart the engine mid-note. Persisted at once:
    // it is the one setting with no Apply button behind it.
    void toggleMute();

    // A run that has just ended: its record, if this run counts towards one,
    // and the summary screen either way. A counted run that fails on an
    // ultra-hard slot deletes the slot instead of recording anything.
    void finishRun(const sim::GameState& game, bool recordProgress);

private:
    // What the confirm screen is asking about, and so what yes does.
    enum class Pending {
        None,
        DeleteSlot,
        AbandonRestart,
        AbandonMainMenu,
        AbandonQuit,
    };

    // The difficulty a run started now is played at. A run with no slot open -
    // one the command line started, or went straight to the level list for -
    // is played at normal.
    Difficulty runDifficulty() const;
    // Leaving the level part way through, which on an ultra-hard slot costs
    // the slot. Only mid-level: once the summary is up, the run is over.
    bool abandoningUltraHard() const;
    // Pushes the open slot's records onto the level list, or clears them when
    // no slot is open.
    void showSlotRecords();
    void deleteSlot(int slot);
    ShellEffect leaveToMainMenu();

    void finishDisplayConfirmation(bool keep);
    // Commits the menu's working copy to the live settings and to disk, and
    // returns whether a display change put the confirmation countdown up. It
    // navigates nowhere: every caller has its own idea of where to go next,
    // and a confirmation screen has already gone somewhere.
    bool applySettingsEdits(double now);

    Window& window_;
    Input& input_;
    audio::Audio& audio_;
    LevelSession& session_;
    LoadedLevel& loaded_;
    PlayerMenu menu_;
    PlayerSettings settings_;
    std::filesystem::path settingsPath_;
    SaveSlots saves_;
    std::filesystem::path savesPath_;
    // -1 while no slot is open. Chosen on the slot screen, and nowhere else.
    int activeSlot_ = -1;
    Pending pending_ = Pending::None;
    bool vsyncAllowed_ = true;
    // What a display change is measured against, and what it is put back to.
    PlayerSettings beforeDisplayChange_;
    bool displayConfirmationPending_ = false;
    double displayConfirmationDeadline_ = 0.0;
};

} // namespace dvk
