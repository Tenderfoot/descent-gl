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

// The player's shell: the menus, the settings they change, the run history they
// build up, and the countdown that puts a display change back if nobody
// confirms it.
//
// It owns the menu, the settings and the progress, because nothing outside it
// writes them. It drives the window, the input, the session and the loaded
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
          PlayerProgress progress, std::filesystem::path progressPath, bool vsyncAllowed);

    PlayerMenu& menu() { return menu_; }
    const PlayerMenu& menu() const { return menu_; }
    const PlayerSettings& settings() const { return settings_; }
    const PlayerProgress& progress() const { return progress_; }

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
    // screens one at a time before it reaches the pause menu itself.
    ShellEffect handlePauseKeys(bool pausePressed, bool escapePressed);

    // Whatever the menu decided this frame.
    ShellEffect applyMenuAction(MenuAction action, double now);

    // Mute keeps the device open and the ambient channels running, so unmuting
    // is instant and does not restart the engine mid-note. Persisted at once:
    // it is the one setting with no Apply button behind it.
    void toggleMute();

    // A run that has just ended: its record, if this run counts towards one,
    // and the summary screen either way.
    void finishRun(const sim::GameState& game, bool recordProgress);

private:
    void finishDisplayConfirmation(bool keep);

    Window& window_;
    Input& input_;
    audio::Audio& audio_;
    LevelSession& session_;
    LoadedLevel& loaded_;
    PlayerMenu menu_;
    PlayerSettings settings_;
    std::filesystem::path settingsPath_;
    PlayerProgress progress_;
    std::filesystem::path progressPath_;
    bool vsyncAllowed_ = true;
    // What a display change is measured against, and what it is put back to.
    PlayerSettings beforeDisplayChange_;
    bool displayConfirmationPending_ = false;
    double displayConfirmationDeadline_ = 0.0;
};

} // namespace dvk
