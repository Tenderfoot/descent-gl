#pragma once

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>

namespace dvk::sim {

struct Body;

inline constexpr int kMaxTriggers = 64;

enum class TriggerShape : uint8_t {
    Segment,
    Sphere,
};

// Immutable, authored trigger data. ScriptHost builds this at level load; the
// fixed step only reads it. A sphere records its containing segment as well as
// its position so two rooms that are spatially close cannot trigger through a
// wall.
struct TriggerVolume {
    TriggerShape shape = TriggerShape::Segment;
    int segment = -1;
    glm::vec3 position{0.0f};
    float radius = 0.0f;
    bool once = true;
};

struct TriggerSet {
    std::array<TriggerVolume, kMaxTriggers> items{};
    int count = 0;
};

// Mutable, copyable simulation state. `inside` gives repeatable triggers true
// enter/exit semantics. The pending list is deduplicated by `latched`, so any
// number of fixed steps before the next rendered frame can enqueue every
// trigger without overflowing or running the same callback twice in one frame.
struct TriggerState {
    std::array<uint8_t, kMaxTriggers> inside{};
    std::array<uint8_t, kMaxTriggers> consumed{};
    std::array<uint8_t, kMaxTriggers> latched{};
    std::array<uint8_t, kMaxTriggers> pending{};
    int pendingCount = 0;

    void clearPending() {
        for (int i = 0; i < pendingCount; ++i) {
            latched[pending[static_cast<size_t>(i)]] = 0;
        }
        pendingCount = 0;
    }
};

// Fixed-step evaluation. This is deliberately a standalone sim function so
// the script test can prove entry/re-entry behavior without constructing the
// rest of the game flow.
void stepTriggers(TriggerState& state, const Body& player, const TriggerSet* triggers);

// --- timers ---------------------------------------------------------------
//
// A moment on the level's own clock rather than a place in the mine: what an
// opening transmission, a delayed repair or a scripted beat wants, none of
// which the player can walk into.
//
// Deliberately NOT a fourth TriggerShape. A trigger is a volume, and every
// field of TriggerVolume - segment, position, radius - is meaningless for a
// moment in time; folding the two together would also mean anything walking
// the trigger set as volumes silently treats a timer as one at segment -1.
// The two live side by side and share nothing but the shape of their latch.

inline constexpr int kMaxTimers = 32;

// Immutable, authored timer data, built by ScriptHost at level load. `at` is
// seconds since the level started. A repeating timer treats `at` as its
// period and comes round again at every multiple of it.
struct TimerEvent {
    float at = 0.0f;
    bool once = true;
};

struct TimerSet {
    std::array<TimerEvent, kMaxTimers> items{};
    int count = 0;
};

// Mutable, copyable simulation state, and copyable is the point: it lives in
// GameState beside TriggerState, so restarting a level rewinds the clock and
// the intro plays again. A clock kept in the script host instead would run
// once per process and never come back.
struct TimerState {
    std::array<uint16_t, kMaxTimers> fired{};
    std::array<uint8_t, kMaxTimers> consumed{};
    std::array<uint8_t, kMaxTimers> latched{};
    std::array<uint8_t, kMaxTimers> pending{};
    int pendingCount = 0;

    void clearPending() {
        for (int i = 0; i < pendingCount; ++i) {
            latched[pending[static_cast<size_t>(i)]] = 0;
        }
        pendingCount = 0;
    }
};

// Fixed-step evaluation against the level's elapsed time. Standalone for the
// same reason stepTriggers is, and latched the same way, so however many
// fixed steps fall between two rendered frames a timer is queued at most once
// across them.
void stepTimers(TimerState& state, float elapsed, const TimerSet* timers);

} // namespace dvk::sim
