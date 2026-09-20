#include "game/soundtrack.h"

#include "audio/audio.h"
#include "game/state.h"
#include "sim/combat.h"
#include "sim/gameflow.h"
#include "sim/robots.h"

#include <algorithm>

namespace dvk {

namespace {

// The loudest thing in the mine that is awake and can still act on it.
float combatAwareness(const SimState& state) {
    float loudest = 0.0f;
    for (int i = 0; i < state.robots.count; ++i) {
        const sim::Robot& robot = state.robots.robots[static_cast<size_t>(i)];
        if (!robot.alive || robot.mode == sim::RobotMode::Disabled) continue;
        loudest = std::max(loudest, robot.awareness);
    }
    return loudest;
}

} // namespace

void updateSoundtrack(MusicDirector& director, audio::Audio& audio, const SimState& state,
                      AppMode mode, bool commDucked, float musicVolume, float frameSeconds,
                      float& marqueeTime) {
    // Either of these means combat whatever the robots are doing.
    const bool forced = state.combat.player.hurtTimer > 0.0f ||
                        state.game.phase == sim::GamePhase::Countdown;

    // Frozen while paused, and this is the same delta the envelopes inside
    // setMusicState run on, so the crossfade stops with it.
    const float delta = mode == AppMode::Playing ? frameSeconds : 0.0f;
    const float target = director.update(combatAwareness(state), forced, delta);

    const bool audible = mode == AppMode::Playing || mode == AppMode::Paused;
    if (audible) {
        audio.setMusicState(target, musicVolume, commDucked, delta);
        marqueeTime += frameSeconds;
        return;
    }
    audio.stopMusic();
    marqueeTime = 0.0f;
    director = MusicDirector{};
}

} // namespace dvk
