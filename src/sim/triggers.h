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

// --- pickups --------------------------------------------------------------
//
// A response to the player taking something out of the mine: a key, a laser
// upgrade, a box of ammunition. What a tutorial about the thing just collected
// wants, and what no volume can express - the pickup is wherever the level
// author put it, and a sphere drawn around it would fire whether the player
// stopped for it or not.
//
// Neither a TriggerVolume nor a TimerEvent for the same reason those two are
// separate from each other: every field of both is meaningless here. What is
// authored is a filter on what was taken, and gameflow already decides when
// that happened.
//
// There is also no `inside` and no `latched`, which is the real difference.
// A volume has to remember whether the player is still in it, because standing
// still would otherwise re-enter it 120 times a second. A pickup is collected
// exactly once per run, so a collection is an event in its own right: however
// many fixed steps fall between two drains, each one can be queued as it
// happens and none of them can arrive twice.

inline constexpr int kMaxPickupHooks = 32;
// How many scripted responses can be waiting when the callbacks are next
// drained. One collection queues one entry per matching hook, so this only
// runs out if a frame's worth of steps collects several pickups that several
// repeating hooks all answer. Anything past the cap is dropped rather than
// written out of bounds.
inline constexpr int kMaxPickupFires = 32;

// Matches every pickup in the mine rather than one kind of them.
inline constexpr int kAnyPickup = -1;

// Immutable, authored hook data, built by ScriptHost at level load. `kind` is
// a `PickupKind` as an int, or kAnyPickup. It is an int and not the enum so
// this header keeps its short include list; gameflow, which owns the pickups,
// casts on the way in.
struct PickupHook {
    int kind = kAnyPickup;
    bool once = true;
};

struct PickupHookSet {
    std::array<PickupHook, kMaxPickupHooks> items{};
    int count = 0;
};

// One queued callback: which hook, and the kind that raised it. The kind is
// carried because a hook that matches everything still wants to be told what
// was just taken, and by the time the callback runs the collection is over.
struct PickupFire {
    uint8_t hook = 0;
    uint8_t kind = 0;
};

// Mutable, copyable simulation state, and copyable for the same reason
// TimerState is: it lives in GameState, so restarting a level forgets which
// one-shot hooks have already been spent and the level plays out again from
// the top.
struct PickupHookState {
    std::array<uint8_t, kMaxPickupHooks> consumed{};
    std::array<PickupFire, kMaxPickupFires> pending{};
    int pendingCount = 0;

    void clearPending() { pendingCount = 0; }
};

// Queues every hook that answers a pickup of `kind`, which is a `PickupKind`
// as an int. Called by gameflow the moment a pickup is collected; standalone
// for the same reason stepTriggers is.
void notePickup(PickupHookState& state, int kind, const PickupHookSet* hooks);

} // namespace dvk::sim
