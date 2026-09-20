#include "game/sim_sets.h"

#include "game/level.h"
#include "game/state.h"
#include "sim/collision.h"
#include "sim/occluders.h"

namespace dvk {

void buildOccluders(const Level& level, const SimState& state, sim::OccluderSet& occluders) {
    occluders.clear();
    occluders.addBody(state.ship.body.position, state.ship.body.radius,
                      {sim::OccluderKind::Ship, -1}, state.ship.body.segment);

    for (int i = 0; i < state.robots.count; ++i) {
        const sim::Robot& robot = state.robots.robots[static_cast<size_t>(i)];
        if (!robot.alive) continue;
        occluders.addBody(robot.body.position, robot.body.radius,
                          {sim::OccluderKind::Robot, i}, robot.body.segment);
    }

    for (int i = 0; i < state.props.floatingCount; ++i) {
        const sim::FloatingLight& ball = state.props.floating[static_cast<size_t>(i)];
        if (!ball.alive) continue;
        occluders.addBody(ball.body.position, ball.body.radius, {sim::OccluderKind::Prop, i},
                          ball.body.segment);
    }

    for (int i = 0; i < state.combat.objects.count &&
                    static_cast<size_t>(i) < level.objectProxies.size();
         ++i) {
        if (!state.combat.objects.alive[static_cast<size_t>(i)]) continue;
        occluders.add(level.objectProxies[static_cast<size_t>(i)],
                      {sim::OccluderKind::Object, i},
                      level.objects[static_cast<size_t>(i)].segment);
    }
}

void buildBodySet(SimState& state, sim::BodySet& bodies) {
    bodies.clear();
    bodies.add(state.ship.body);
    for (int i = 0; i < state.robots.count; ++i) {
        sim::Robot& robot = state.robots.robots[static_cast<size_t>(i)];
        if (robot.alive) bodies.add(robot.body);
    }
    for (int i = 0; i < state.props.floatingCount; ++i) {
        sim::FloatingLight& ball = state.props.floating[static_cast<size_t>(i)];
        if (ball.alive) bodies.add(ball.body, ball.restitution);
    }
}

} // namespace dvk
