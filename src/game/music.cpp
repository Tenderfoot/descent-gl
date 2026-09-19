#include "game/music.h"

#include <algorithm>
#include <cstdio>

namespace dvk {

namespace {

// One over the three seconds a robot keeps hunting after losing sight of the
// player (sim/robots.cpp). Held here as the ceiling on how far awareness can
// legitimately fall in a single frame.
constexpr float kAwarenessDecayPerSecond = 1.0f / 3.0f;

// Slack on that ceiling, worth a couple of simulation steps at 120 Hz. The
// steps that ran this frame come out of an accumulator and do not add up to
// exactly the frame delta, so the comparison needs a little room - without it,
// a frame whose accumulator happened to run long would read as a kill.
constexpr float kDecaySlack = 0.01f;

bool expect(bool condition, const char* message) {
    std::printf("music-test: %s %s\n", condition ? "PASS" : "FAIL", message);
    return condition;
}

}

float MusicDirector::update(float awareness, bool forced, float dt) {
    awareness = std::max(awareness, 0.0f);
    dt = std::max(dt, 0.0f);

    const bool inCombat = awareness > 0.0f || forced;
    if (inCombat) {
        // Nothing to grieve while the fight is still on.
        grace = 0.0f;
    } else if (grace > 0.0f) {
        grace = std::max(0.0f, grace - dt);
    } else if (previousAwareness > dt * kAwarenessDecayPerSecond + kDecaySlack) {
        // Awareness only reaches zero by decaying, and it decays slowly enough
        // that the last frame before it gets there is a hair above zero. A
        // value too big for this frame's decay to have consumed did not decay
        // at all: the robot holding it died or was disabled and took its
        // awareness with it. That is the fight ending on a kill rather than on
        // the player getting away, and the crossfade starting underneath the
        // explosion is not the sound of it - so combat is held a moment first.
        grace = kCombatGraceSeconds;
    }

    previousAwareness = awareness;
    return inCombat || grace > 0.0f ? 1.0f : 0.0f;
}

MusicReadout musicReadout(float mix, bool playing, const char* lullName,
                          const char* combatName) {
    MusicReadout readout;
    if (!playing) return readout;

    if (mix <= 0.0f) {
        readout.text = lullName ? lullName : "";
    } else if (mix >= 1.0f) {
        readout.text = combatName ? combatName : "";
    } else {
        readout.text = "transitioning";
        readout.transitioning = true;
    }
    readout.show = readout.text[0] != '\0';
    return readout;
}

int runMusicTest() {
    bool ok = true;
    constexpr float kStep = 1.0f / 120.0f;

    // Nothing is happening, so nothing is playing but the lull.
    {
        MusicDirector music;
        ok &= expect(music.update(0.0f, false, kStep) == 0.0f, "a quiet mine stays in the lull");
        ok &= expect(music.update(1.0f, false, kStep) == 1.0f,
                     "a robot acquiring the player asks for combat at once");
    }

    // The player breaks line of sight and waits it out. Awareness decays over
    // three seconds, and the crossfade starts the moment it reaches zero -
    // there is no grace, because the fight faded rather than ended.
    {
        MusicDirector music;
        music.update(1.0f, false, kStep);
        float awareness = 1.0f;
        float seconds = 0.0f;
        bool alwaysBinary = true;
        while (awareness > 0.0f) {
            awareness = std::max(0.0f, awareness - kStep * kAwarenessDecayPerSecond);
            const float target = music.update(awareness, false, kStep);
            alwaysBinary = alwaysBinary && (target == 0.0f || target == 1.0f);
            if (target == 0.0f) break;
            seconds += kStep;
        }
        ok &= expect(alwaysBinary, "the target is never a partial mix while awareness decays");
        ok &= expect(seconds > 2.9f && seconds < 3.1f,
                     "breaking line of sight holds combat for awareness's three second decay");
        ok &= expect(music.grace == 0.0f, "a fight that faded out gets no grace");
    }

    // The same fight ended on a kill instead: awareness vanishes with the robot
    // rather than decaying, and the combat track is held for two seconds.
    {
        MusicDirector music;
        music.update(1.0f, false, kStep);
        ok &= expect(music.update(0.0f, false, kStep) == 1.0f,
                     "killing the last aware robot does not drop the music on the explosion");
        float seconds = kStep;
        while (music.update(0.0f, false, kStep) == 1.0f && seconds < 10.0f) seconds += kStep;
        ok &= expect(seconds > 1.95f && seconds < 2.10f,
                     "the grace after a kill is two seconds");
        ok &= expect(music.update(0.0f, false, kStep) == 0.0f,
                     "the lull returns once the grace is spent");
    }

    // A robot waking up inside the grace takes it straight back to combat, and
    // spends the grace doing it: the next fade-out is the ordinary one.
    {
        MusicDirector music;
        music.update(1.0f, false, kStep);
        music.update(0.0f, false, kStep);
        for (int i = 0; i < 60; ++i) music.update(0.0f, false, kStep);
        ok &= expect(music.grace > 0.0f, "the grace is still running half a second in");
        ok &= expect(music.update(1.0f, false, kStep) == 1.0f,
                     "a robot re-acquiring inside the grace returns to combat");
        ok &= expect(music.grace == 0.0f, "returning to combat clears the grace");
    }

    // A pause is not two seconds of the fight being over.
    {
        MusicDirector music;
        music.update(1.0f, false, kStep);
        music.update(0.0f, false, kStep);
        const float held = music.grace;
        for (int i = 0; i < 600; ++i) music.update(0.0f, false, 0.0f);
        ok &= expect(music.grace == held, "a paused game freezes the grace");
        ok &= expect(music.update(0.0f, false, 0.0f) == 1.0f, "and holds combat while it does");
    }

    // Damage and the countdown mean combat whatever the robots are doing, and
    // they hold it directly rather than through the grace.
    {
        MusicDirector music;
        ok &= expect(music.update(0.0f, true, kStep) == 1.0f,
                     "damage and the countdown force combat with no robot aware");
        ok &= expect(music.grace == 0.0f, "forced combat needs no grace to hold it");
        ok &= expect(music.update(0.0f, false, kStep) == 0.0f,
                     "and releases as soon as it stops being forced");
    }

    // The frame rate must not change what any of this means. A software
    // renderer's frame is a quarter of a second, which is most of one step of
    // the decay - long enough to be mistaken for a kill if the ceiling were a
    // fixed number rather than this frame's worth of decay.
    {
        constexpr float kSlowFrame = 0.25f;
        MusicDirector music;
        music.update(1.0f, false, kSlowFrame);
        float awareness = 1.0f;
        while (awareness > 0.0f) {
            awareness = std::max(0.0f, awareness - kSlowFrame * kAwarenessDecayPerSecond);
            music.update(awareness, false, kSlowFrame);
        }
        ok &= expect(music.grace == 0.0f, "a decay seen at four frames a second is not a kill");

        MusicDirector killed;
        killed.update(1.0f, false, kSlowFrame);
        ok &= expect(killed.update(0.0f, false, kSlowFrame) == 1.0f &&
                         killed.grace == kCombatGraceSeconds,
                     "a kill seen at four frames a second still is one");
    }

    std::printf("music test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

}
