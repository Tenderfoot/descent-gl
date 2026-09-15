#include "sim/props.h"

#include "core/log.h"
#include "game/level.h"
#include "sim/collision.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace dvk::sim {

namespace {

// Deterministic, and owned here rather than drawn from a std::mt19937: a ball's
// wander has to be reproducible from a bug report like everything else.
uint32_t g_wander = 0;

float wanderUnit() {
    g_wander = g_wander * 1664525u + 1013904223u;
    return static_cast<float>((g_wander >> 8) & 0xFFFFFFu) / 16777215.0f * 2.0f - 1.0f;
}

}   // namespace

glm::vec3 lampPosition(const SwingingLight& lamp, float angle) {
    const glm::vec3 axis = glm::length(lamp.swingAxis) > 1e-5f ? glm::normalize(lamp.swingAxis)
                                                               : glm::vec3(1.0f, 0.0f, 0.0f);
    // Straight down, tilted by `angle` along the swing axis. The cord stays
    // taut by construction, which is the point of doing it this way.
    const glm::vec3 down(0.0f, -1.0f, 0.0f);
    return lamp.anchor + (down * std::cos(angle) + axis * std::sin(angle)) * lamp.cordLength;
}

void spawnProps(const Level& level, PropState& out) {
    out = PropState{};

    for (const FloatingLightDef& def : level.floatingLights) {
        if (out.floatingCount >= kMaxFloatingLights) {
            log::warn("more than {} floating lights in the level; the rest are ignored",
                      kMaxFloatingLights);
            break;
        }
        FloatingLight& ball = out.floating[static_cast<size_t>(out.floatingCount++)];
        ball = FloatingLight{};
        ball.alive = true;
        ball.body.position = def.position;
        ball.body.segment = def.segment;
        ball.body.radius = def.radius;
        ball.color = def.color;
        ball.lightRadius = def.lightRadius;
        ball.lightIntensity = def.lightIntensity;
        ball.restitution = def.restitution;
        ball.mass = def.mass;
        ball.drift = def.drift;
    }

    for (const SwingingLightDef& def : level.swingingLights) {
        if (out.swingingCount >= kMaxSwingingLights) {
            log::warn("more than {} swinging lights in the level; the rest are ignored",
                      kMaxSwingingLights);
            break;
        }
        SwingingLight& lamp = out.swinging[static_cast<size_t>(out.swingingCount++)];
        lamp = SwingingLight{};
        lamp.alive = true;
        lamp.anchor = def.anchor;
        lamp.segment = def.segment;
        lamp.cordLength = def.cordLength;
        lamp.amplitude = def.amplitude;
        lamp.rate = def.rate;
        lamp.phase = def.phase;
        lamp.swingAxis = def.swingAxis;
        lamp.color = def.color;
        lamp.lightRadius = def.lightRadius;
        lamp.lightIntensity = def.lightIntensity;
        lamp.coneDegrees = def.coneDegrees;
        // Placed where it will actually be on the first frame, so nothing ever
        // sees it hanging straight down at the origin of its swing.
        const float angle = lamp.amplitude * std::sin(lamp.phase);
        lamp.position = lampPosition(lamp, angle);
        lamp.direction = glm::normalize(lamp.position - lamp.anchor);
    }

    if (out.floatingCount > 0 || out.swingingCount > 0) {
        log::info("{} floating light(s), {} swinging light(s)", out.floatingCount,
                  out.swingingCount);
    }
}

void stepProps(const Level& level, PropState& state, float dt, const SolidDoors* doors) {
    CollisionConfig bounce;
    // Both of these are what make a ball bounce rather than slide to a stop
    // along the wall it hit. See CollisionConfig.
    bounce.scrapeDamping = 0.0f;

    for (int i = 0; i < state.floatingCount; ++i) {
        FloatingLight& ball = state.floating[static_cast<size_t>(i)];
        if (!ball.alive) continue;

        // A new wander heading every couple of seconds, eased into rather than
        // snapped to, so an untouched ball drifts instead of darting.
        ball.driftTimer -= dt;
        if (ball.driftTimer <= 0.0f) {
            ball.driftTimer = 1.8f + wanderUnit() * 0.6f;
            ball.driftAim = glm::vec3(wanderUnit(), wanderUnit() * 0.5f, wanderUnit());
            const float length = glm::length(ball.driftAim);
            ball.driftAim = length > 1e-4f ? ball.driftAim * (ball.drift / length) : glm::vec3(0.0f);
        }
        ball.body.velocity += (ball.driftAim - ball.body.velocity) * std::min(1.0f, dt * 0.7f);

        // The shove from being shot lives in the knockback channel and is what
        // makes the ball bounce; it bleeds off at the shared rate.
        ball.body.knockback *= std::exp(-kKnockbackDamping * dt);
        bounce.restitution = ball.restitution;

        // Integrated and resolved once, like every other body in the mine. A
        // shove of 150 u/s - past anything the weapons produce against a 0.45
        // mass - was run from every floating light in all 26 directions for
        // five seconds each and never left the mine, so the sub-stepping this
        // looked like it would need is complexity the evidence does not
        // support. See --world-test.
        ball.body.position += ball.body.motion() * dt;
        resolveCollision(level, ball.body, bounce, dt, nullptr, doors);
    }

    for (int i = 0; i < state.swingingCount; ++i) {
        SwingingLight& lamp = state.swinging[static_cast<size_t>(i)];
        if (!lamp.alive) continue;

        lamp.time += dt;
        const float angle = lamp.amplitude * std::sin(lamp.time * lamp.rate + lamp.phase);
        lamp.position = lampPosition(lamp, angle);
        // The lamp points the way the cord does. A real lamp housing hangs
        // along its cord, so the spot sweeps the floor as the lamp swings
        // rather than staring straight down from a moving point.
        const glm::vec3 along = lamp.position - lamp.anchor;
        const float length = glm::length(along);
        lamp.direction = length > 1e-5f ? along / length : glm::vec3(0.0f, -1.0f, 0.0f);
    }
}

} // namespace dvk::sim
