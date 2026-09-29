#include "game/shell.h"

#include "audio/audio.h"
#include "core/input.h"
#include "core/window.h"
#include "game/comm_director.h"
#include "game/level_load.h"
#include "game/script.h"
#include "game/session.h"
#include "game/tutorial.h"
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
    audio_.setEffectsGain(settings_.effectsVolume);
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
    // Back to the screen the change was made on. Only the video screen can
    // make one, so there is no question of which that was, and landing there
    // is what lets a rejected resolution be corrected on the spot.
    menu_.showSettingsScreen(MenuScreen::SettingsVideo);
}

bool Shell::applySettingsEdits(double now) {
    const PlayerSettings previous = settings_;
    settings_ = menu_.editedSettings();
    // A display mode that does not come back needs a way out that does not
    // require being able to see the screen, so a change to one stands only
    // until the countdown runs out.
    const bool displayChanged = settings_.fullscreen != previous.fullscreen ||
                                settings_.windowWidth != previous.windowWidth ||
                                settings_.windowHeight != previous.windowHeight ||
                                settings_.vsync != previous.vsync;
    beforeDisplayChange_ = previous;
    applyRuntimeSettings();
    if (!displayChanged) {
        savePlayerSettings(settingsPath_, settings_);
        return false;
    }
    displayConfirmationPending_ = true;
    displayConfirmationDeadline_ = now + kDisplayConfirmationSeconds;
    menu_.showDisplayConfirmation(static_cast<int>(kDisplayConfirmationSeconds));
    return true;
}

void Shell::tickDisplayConfirmation(double now) {
    if (!displayConfirmationPending_) return;
    const int seconds =
        std::max(0, static_cast<int>(std::ceil(displayConfirmationDeadline_ - now)));
    menu_.setConfirmationSeconds(seconds);
    if (now >= displayConfirmationDeadline_) finishDisplayConfirmation(false);
}

ShellEffect Shell::handlePauseKeys(bool pausePressed, bool escapePressed, double now) {
    ShellEffect effect;
    if (capturingBinding()) return effect;
    // A tutorial pane is modal on purpose. It stopped the game to say
    // something, and a key pressed by reflex on the way past would dismiss it
    // unread; the okay button is the only way out of one.
    if (menu_.screen() == MenuScreen::Tutorial) return effect;

    const bool eitherKey = pausePressed || escapePressed;
    if (eitherKey && menu_.screen() == MenuScreen::DisplayConfirmation) {
        finishDisplayConfirmation(false);
        effect.resetAccumulator = true;
    } else if (eitherKey && menu_.screen() == MenuScreen::Controls) {
        // The bindings are edited into the same working copy as everything
        // else, so leaving this screen commits them the way leaving any other
        // settings screen commits its values.
        if (!applySettingsEdits(now)) menu_.closeControls();
        effect.resetAccumulator = true;
    } else if (eitherKey && editsSettings(menu_.screen())) {
        if (!applySettingsEdits(now)) menu_.closeSettings();
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
            menu_.showLevelGroups();
            break;
        case MenuAction::ChooseCampaign:
            menu_.showLevelSelect(LevelGroup::Campaign);
            break;
        case MenuAction::ChooseOtherMaps:
            menu_.showLevelSelect(LevelGroup::Other);
            break;
        case MenuAction::OpenSettings:
            menu_.showSettings(settings_);
            break;
        case MenuAction::OpenVideoSettings:
            menu_.showSettingsScreen(MenuScreen::SettingsVideo);
            break;
        case MenuAction::OpenAudioSettings:
            menu_.showSettingsScreen(MenuScreen::SettingsAudio);
            break;
        case MenuAction::OpenGameSettings:
            menu_.showSettingsScreen(MenuScreen::SettingsGame);
            break;
        case MenuAction::OpenControls:
            menu_.showControls();
            break;
        case MenuAction::ApplySettings:
            // Applies and stays put. Leaving commits too, so this is the
            // discoverable half of that rather than the only way to commit -
            // and staying is what lets a new resolution be looked at and
            // changed again without walking back in.
            applySettingsEdits(now);
            effect.resetAccumulator = true;
            break;
        case MenuAction::KeepDisplay:
            finishDisplayConfirmation(true);
            break;
        case MenuAction::RevertDisplay:
            finishDisplayConfirmation(false);
            break;
        case MenuAction::Back:
            // Backing out of a screen that edits something commits it. There
            // is no cancel: a settings screen shows what it is going to do to
            // the game as you move through it, so the only way to leave one is
            // with what it is showing.
            if (menu_.screen() == MenuScreen::Controls) {
                if (!applySettingsEdits(now)) menu_.closeControls();
            } else if (editsSettings(menu_.screen())) {
                if (!applySettingsEdits(now)) menu_.closeSettings();
            } else if (menu_.screen() == MenuScreen::Settings) {
                menu_.closeSettings();
            } else if (menu_.screen() == MenuScreen::LevelSelect) {
                menu_.showLevelGroups();
            } else {
                menu_.showMain();
            }
            effect.resetAccumulator = true;
            break;
        case MenuAction::Start:
            if (menu_.selectedLevel() >= 0) {
                loadLevelFile(loaded_,
                              menu_.levels()[static_cast<size_t>(menu_.selectedLevel())].path);
            }
            loaded_.tutorial.clear();
            loadLevelScript(loaded_.script, loaded_.level, loaded_.comm);
            session_.start();
            loaded_.comm.enterLevel();
            audio_.shuffleMusicTracks();
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
            // Before the reload, not after: the pane's heading and body are
            // pointers into the script that is about to be thrown away.
            loaded_.tutorial.clear();
            loadLevelScript(loaded_.script, loaded_.level, loaded_.comm);
            session_.restart();
            loaded_.comm.enterLevel();
            audio_.advanceMusicTracks();
            menu_.hide();
            loaded_.contacts.clear();
            loaded_.automap.open = false;
            loaded_.lightDoorSignature = -1;
            captureMouse(true);
            effect.resetAccumulator = true;
            break;
        case MenuAction::MainMenu:
            loaded_.tutorial.clear();
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
        case MenuAction::AcknowledgeTutorial:
            loaded_.tutorial.dismiss();
            session_.resume();
            menu_.hide();
            captureMouse(true);
            effect.resetAccumulator = true;
            effect.resyncPrevious = true;
            break;
        case MenuAction::None:
            break;
    }
    return effect;
}

ShellEffect Shell::showTutorial(const ScriptTutorial& tutorial, double now) {
    ShellEffect effect;
    if (!settings_.tutorials) return effect;
    // Only over a run in progress. A pane raised while the game is already
    // stopped would be stacked on a menu it cannot be dismissed back to, and
    // one raised over the end-of-level card would stop a level that is over.
    if (session_.mode() != AppMode::Playing) return effect;

    // A transmission and a tutorial share the one comm voice, and the
    // transmission's own clock keeps running behind a stopped simulation, so
    // one left playing would talk over the pane and then retire itself while
    // the player was reading. The game is stopping; so is it.
    loaded_.comm.stop();
    loaded_.tutorial.show(tutorial, now);
    session_.pause();
    menu_.showTutorial(tutorial.title.c_str(), tutorial.text.c_str());
    loaded_.automap.open = false;
    captureMouse(false);
    effect.resetAccumulator = true;
    effect.resyncPrevious = true;
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
