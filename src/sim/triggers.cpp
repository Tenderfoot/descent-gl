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

} // namespace dvk::sim
