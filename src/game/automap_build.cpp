#include "game/automap_build.h"

#include "game/level.h"
#include "game/state.h"
#include "render/renderer.h"
#include "sim/gameflow.h"
#include "sim/robots.h"

#include <algorithm>

namespace dvk {

namespace {

void addMarker(gl::AutomapData& map, const glm::vec3& position, const glm::vec3& color,
               float size, bool hollow) {
    if (map.markerCount >= gl::AutomapData::kMaxMarkers) return;
    gl::AutomapData::Marker& marker = map.markers[static_cast<size_t>(map.markerCount++)];
    marker.position = position;
    marker.color = color;
    marker.size = size;
    marker.hollow = hollow;
}

glm::vec3 keyColor(PickupKind kind) {
    switch (kind) {
        case PickupKind::KeyBlue: return glm::vec3(0.35f, 0.55f, 1.0f);
        case PickupKind::KeyYellow: return glm::vec3(1.0f, 0.9f, 0.3f);
        case PickupKind::KeyRed: return glm::vec3(1.0f, 0.35f, 0.3f);
        default: break;
    }
    return glm::vec3(1.0f);
}

// Keys the player has laid eyes on, whether or not they picked them up.
// Anything else on the map would be telling them where to go.
void addKeys(gl::AutomapData& map, const Level& level, const SimState& state, bool reveal) {
    const size_t count =
        std::min(level.pickups.size(), static_cast<size_t>(sim::kMaxPickups));
    for (size_t i = 0; i < count; ++i) {
        const Pickup& pickup = level.pickups[i];
        if (pickup.kind < PickupKind::KeyBlue) continue;
        if (!reveal && !state.game.seen[i]) continue;
        const bool collected = state.game.collected[i];
        addMarker(map, pickup.position, keyColor(pickup.kind), 1.8f, !collected);
    }
}

void addReactor(gl::AutomapData& map, const SimState& state, const sim::RobotConfig& robots,
                bool reveal) {
    if (!state.game.reactorSeen && !reveal) return;
    // Green once it is gone, so a glance says whether the job is done and the
    // map is now an escape route.
    const glm::vec3 color = state.game.reactorAlive ? glm::vec3(1.0f, 0.45f, 0.2f)
                                                    : glm::vec3(0.4f, 1.0f, 0.5f);
    glm::vec3 at = state.game.reactorPosition;
    if (reveal && !state.game.reactorSeen) {
        for (int i = 0; i < state.robots.count; ++i) {
            const sim::Robot& robot = state.robots.robots[static_cast<size_t>(i)];
            if (robots.types[static_cast<size_t>(robot.type)].isReactor) {
                at = robot.body.position;
            }
        }
    }
    addMarker(map, at, color, 3.0f, false);
}

} // namespace

void buildAutomap(gl::AutomapData& map, const Level& level, const SimState& state,
                  const RenderState& render, const glm::vec3& shipForward,
                  const sim::RobotConfig& robots, bool reveal) {
    map.segmentCount = level.segmentCount();
    map.visitedCount = reveal ? level.segmentCount() : state.game.visitedCount;
    for (int i = 0; i < map.segmentCount && i < gl::AutomapData::kMaxSegments; ++i) {
        map.visited[static_cast<size_t>(i)] =
            reveal || state.game.visited[static_cast<size_t>(i)];
    }
    map.shipPosition = render.shipPos;
    map.shipForward = shipForward;
    map.shipSegment = render.shipSegment;

    map.markerCount = 0;
    addKeys(map, level, state, reveal);
    addReactor(map, state, robots, reveal);
    if (state.game.exitOpen && level.exitTrigger.present) {
        addMarker(map, level.exitTrigger.position, glm::vec3(0.5f, 1.0f, 0.7f), 3.0f, false);
    }
}

} // namespace dvk
