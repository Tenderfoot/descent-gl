#include "game/shell.h"

#include "audio/audio.h"
#include "core/input.h"
#include "core/window.h"
#include "game/comm_director.h"
#include "game/level_load.h"
#include "game/script.h"
#include "game/session.h"
#include "render/renderer.h"
#include "sim/collision.h"
#include "sim/gameflow.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace dvk {

Shell::Shell(Window& window, Input& input, audio::Audio& audio, LevelSession& session,
             LoadedLevel& loaded, PlayerSettings settings, std::filesystem::path settingsPath,
             PlayerProgress progress, std::filesystem::path progressPath, bool vsyncAllowed)
    : window_(window),
      input_(input),
      audio_(audio),
      session_(session),
      loaded_(loaded),
      settings_(std::move(settings)),
      settingsPath_(std::move(settingsPath)),
      progress_(std::move(progress)),
      progressPath_(std::move(progressPath)),
      vsyncAllowed_(vsyncAllowed),
      beforeDisplayChange_(settings_) {}

void Shell::applyAudioSettings() {
    audio_.setMasterGain(settings_.masterVolume);
    audio_.setMuted(settings_.muted);
}

void Shell::applyRuntimeSettings() {
    applyAudioSettings();
    window_.setVsync(settings_.vsync && vsyncAllowed_);
    window_.setDisplay(settings_.fullscreen, settings_.windowWidth, settings_.windowHeight);
}

void Shell::captureMouse(bool captured) {
    input_.setMouseCaptured(window_.handle(), captured);
}

bool Shell::capturingBinding() const {
    return menu_.screen() == MenuScreen::Controls && menu_.capturingBinding();
}

void Shell::finishDisplayConfirmation(bool keep) {
    if (!keep) {
        settings_.fullscreen = beforeDisplayChange_.fullscreen;
        settings_.windowWidth = beforeDisplayChange_.windowWidth;
        settings_.windowHeight = beforeDisplayChange_.windowHeight;
        settings_.vsync = beforeDisplayChange_.vsync;
        applyRuntimeSettings();
    }
    savePlayerSettings(settingsPath_, settings_);
    displayConfirmationPending_ = false;
    menu_.closeSettings();
}

void Shell::tickDisplayConfirmation(double now) {
    if (!displayConfirmationPending_) return;
    const int seconds =
        std::max(0, static_cast<int>(std::ceil(displayConfirmationDeadline_ - now)));
    menu_.setConfirmationSeconds(seconds);
    if (now >= displayConfirmationDeadline_) finishDisplayConfirmation(false);
}

ShellEffect Shell::handlePauseKeys(bool pausePressed, bool escapePressed) {
    ShellEffect effect;
    if (capturingBinding()) return effect;

    const bool eitherKey = pausePressed || escapePressed;
    if (eitherKey && menu_.screen() == MenuScreen::DisplayConfirmation) {
        finishDisplayConfirmation(false);
        effect.resetAccumulator = true;
    } else if (eitherKey && menu_.screen() == MenuScreen::Controls) {
        menu_.closeControls();
        effect.resetAccumulator = true;
    } else if (eitherKey && menu_.screen() == MenuScreen::Settings) {
        menu_.closeSettings();
        effect.resetAccumulator = true;
    } else if (pausePressed) {
        if (session_.mode() == AppMode::Playing) {
            session_.pause();
            menu_.showPause();
            loaded_.automap.open = false;
            captureMouse(false);
        } else if (session_.mode() == AppMode::Paused) {
            session_.resume();
            menu_.hide();
            captureMouse(true);
        }
        effect.resetAccumulator = true;
        effect.resyncPrevious = true;
    }
    return effect;
}

ShellEffect Shell::applyMenuAction(MenuAction action, double now) {
    ShellEffect effect;
    switch (action) {
        case MenuAction::ChooseLevel:
            menu_.showLevelSelect();
            break;
        case MenuAction::OpenSettings:
            menu_.showSettings(settings_);
            break;
        case MenuAction::OpenControls:
            menu_.showControls();
            break;
        case MenuAction::ApplySettings: {
            const PlayerSettings previous = settings_;
            settings_ = menu_.editedSettings();
            // A display mode that does not come back needs a way out that does
            // not require being able to see the screen, so a change to one
            // stands only until the countdown runs out.
            const bool displayChanged = settings_.fullscreen != previous.fullscreen ||
                                        settings_.windowWidth != previous.windowWidth ||
                                        settings_.windowHeight != previous.windowHeight ||
                                        settings_.vsync != previous.vsync;
            beforeDisplayChange_ = previous;
            applyRuntimeSettings();
            if (displayChanged) {
                displayConfirmationPending_ = true;
                displayConfirmationDeadline_ = now + kDisplayConfirmationSeconds;
                menu_.showDisplayConfirmation(static_cast<int>(kDisplayConfirmationSeconds));
            } else {
                savePlayerSettings(settingsPath_, settings_);
                menu_.closeSettings();
            }
            effect.resetAccumulator = true;
            break;
        }
        case MenuAction::KeepDisplay:
            finishDisplayConfirmation(true);
            break;
        case MenuAction::RevertDisplay:
            finishDisplayConfirmation(false);
            break;
        case MenuAction::Back:
            if (menu_.screen() == MenuScreen::Controls) {
                menu_.closeControls();
            } else if (menu_.screen() == MenuScreen::Settings) {
                menu_.closeSettings();
            } else {
                menu_.showMain();
            }
            break;
        case MenuAction::Start:
            if (menu_.selectedLevel() >= 0) {
                loadLevelFile(loaded_,
                              menu_.levels()[static_cast<size_t>(menu_.selectedLevel())].path);
            }
            loadLevelScript(loaded_.script, loaded_.level, loaded_.comm);
            session_.start();
            loaded_.comm.enterLevel();
            menu_.hide();
            loaded_.lightDoorSignature = -1;
            captureMouse(true);
            effect.resetAccumulator = true;
            break;
        case MenuAction::Resume:
            session_.resume();
            menu_.hide();
            captureMouse(true);
            effect.resetAccumulator = true;
            effect.resyncPrevious = true;
            break;
        case MenuAction::Restart:
            loadLevelScript(loaded_.script, loaded_.level, loaded_.comm);
            session_.restart();
            loaded_.comm.enterLevel();
            menu_.hide();
            loaded_.contacts.clear();
            loaded_.automap.open = false;
            loaded_.lightDoorSignature = -1;
            captureMouse(true);
            effect.resetAccumulator = true;
            break;
        case MenuAction::MainMenu:
            session_.leave();
            menu_.showMain();
            loaded_.contacts.clear();
            loaded_.automap.open = false;
            captureMouse(false);
            effect.resetAccumulator = true;
            effect.resyncPrevious = true;
            break;
        case MenuAction::Quit:
            window_.requestClose();
            break;
        case MenuAction::None:
            break;
    }
    return effect;
}

void Shell::toggleMute() {
    audio_.setMuted(!audio_.muted());
    settings_.muted = audio_.muted();
    savePlayerSettings(settingsPath_, settings_);
}

void Shell::finishRun(const sim::GameState& game, bool recordProgress) {
    const bool won = game.phase == sim::GamePhase::Won;
    const char* note = "";
    if (recordProgress) {
        const std::string levelId = levelProgressId(loaded_.path);
        const RunRecordUpdate update =
            recordRun(progress_, levelId, won, game.elapsed, game.robotsDestroyed);
        savePlayerProgress(progressPath_, progress_);
        if (const LevelRecord* record = progress_.find(levelId)) {
            menu_.setLevelRecord(loaded_.path, record->wins, static_cast<int>(record->bestTime));
        }
        note = runRecordNote(update);
    }
    menu_.showSummary(won, game.endReason, game.elapsed, game.robotsDestroyed,
                      game.robotsAtStart, note);
}

} // namespace dvk
