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

} // namespace dvk::sim
