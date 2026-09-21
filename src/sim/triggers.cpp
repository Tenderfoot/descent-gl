#include "sim/triggers.h"

#include "sim/body.h"

#include <glm/geometric.hpp>

#include <algorithm>

namespace dvk::sim {

void stepTriggers(TriggerState& state, const Body& player, const TriggerSet* triggers) {
    if (!triggers) return;

    const int count = std::clamp(triggers->count, 0, kMaxTriggers);
    for (int i = 0; i < count; ++i) {
        const TriggerVolume& trigger = triggers->items[static_cast<size_t>(i)];
        const bool inSegment = player.segment == trigger.segment;
        bool inside = inSegment;
        if (trigger.shape == TriggerShape::Sphere) {
            const glm::vec3 delta = player.position - trigger.position;
            inside = inSegment && glm::dot(delta, delta) <= trigger.radius * trigger.radius;
        }

        const size_t ti = static_cast<size_t>(i);
        const bool entered = inside && state.inside[ti] == 0;
        state.inside[ti] = inside ? 1u : 0u;
        if (!entered || state.consumed[ti] != 0 || state.latched[ti] != 0) continue;

        state.latched[ti] = 1;
        state.pending[static_cast<size_t>(state.pendingCount++)] = static_cast<uint8_t>(i);
        if (trigger.once) state.consumed[ti] = 1;
    }
}

void stepTimers(TimerState& state, float elapsed, const TimerSet* timers) {
    if (!timers) return;

    const int count = std::clamp(timers->count, 0, kMaxTimers);
    for (int i = 0; i < count; ++i) {
        const TimerEvent& timer = timers->items[static_cast<size_t>(i)];
        const size_t ti = static_cast<size_t>(i);
        if (state.consumed[ti] != 0 || state.latched[ti] != 0) continue;

        // A repeating timer's nth firing is due at n periods, counted from the
        // level's start rather than from the last firing. Accumulating a delta
        // instead would let a timer drift by however long a frame hitched.
        const float due = timer.at * static_cast<float>(state.fired[ti] + 1);
        if (elapsed < due) continue;

        if (state.fired[ti] < 0xFFFFu) ++state.fired[ti];
        state.latched[ti] = 1;
        state.pending[static_cast<size_t>(state.pendingCount++)] = static_cast<uint8_t>(i);
        if (timer.once) state.consumed[ti] = 1;
    }
}

void notePickup(PickupHookState& state, int kind, const PickupHookSet* hooks) {
    if (!hooks) return;

    const int count = std::clamp(hooks->count, 0, kMaxPickupHooks);
    for (int i = 0; i < count; ++i) {
        const PickupHook& hook = hooks->items[static_cast<size_t>(i)];
        const size_t hi = static_cast<size_t>(i);
        if (state.consumed[hi] != 0) continue;
        if (hook.kind != kAnyPickup && hook.kind != kind) continue;
        if (state.pendingCount >= kMaxPickupFires) return;

        state.pending[static_cast<size_t>(state.pendingCount++)] = {
            static_cast<uint8_t>(i), static_cast<uint8_t>(kind)};
        if (hook.once) state.consumed[hi] = 1;
    }
}

} // namespace dvk::sim
