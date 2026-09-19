#pragma once

namespace dvk {

// How long the combat soundtrack outlives a fight that ended on a kill rather
// than on the player getting away. See MusicDirector::update.
inline constexpr float kCombatGraceSeconds = 2.0f;

// Decides whether the soundtrack should be in combat, from what the simulation
// last reported. Presentation state, derived from the simulation and never fed
// back into it, which is why this lives here and not under src/sim.
//
// The answer is all or nothing. A partial value leaves both tracks audible
// against each other for as long as it lasts, and the two are meant to cross,
// not to play together - so the only time either is heard over the other is the
// crossfade itself.
struct MusicDirector {
    // Counting down while the fight is over but the music is not. Zero when
    // nothing is holding it.
    float grace = 0.0f;
    // The highest awareness the last frame saw. What it was matters, because
    // it is what says whether the fight faded out or was ended: see update().
    float previousAwareness = 0.0f;

    // `awareness` is the highest awareness of any living, enabled robot;
    // `forced` is player damage or the reactor countdown, either of which means
    // combat whatever the robots are doing. `dt` is the frame delta, and is
    // zero while paused so the grace freezes with everything else.
    //
    // Returns the target for Audio::setMusicState: 1 for combat, 0 for the lull.
    float update(float awareness, bool forced, float dt);
};

int runMusicTest();

}
