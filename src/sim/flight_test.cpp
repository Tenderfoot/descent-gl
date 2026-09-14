#include "sim/flight_test.h"

#include "core/log.h"
#include "sim/flight.h"
#include "sim/ship.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>

namespace dvk::sim {

namespace {

constexpr float kStep = 1.0f / 120.0f;
constexpr float kMaxSeconds = 10.0f;

// "Visually stopped" is a judgement call, so it is written down here rather
// than left implicit: a twentieth of top speed, which at the shipped tuning is
// 1 unit/s, is slow enough that the drift is not readable on screen.
constexpr float kStoppedFraction = 0.05f;

// A full swipe across a mouse pad, for the turn measurement.
constexpr float kSwipePixels = 800.0f;

bool inRange(float value, float lo, float hi) { return value >= lo && value <= hi; }

} // namespace

int runFlightTest(const char* configPath) {
    const FlightConfig cfg = loadFlightConfig(configPath);
    bool ok = true;

    // --- spin up ---------------------------------------------------------
    ShipState s;
    ShipControls full;
    full.thrust = 1.0f;

    const float terminal = std::min(cfg.forwardThrust / cfg.linearDamping, cfg.maxSpeed);
    const float target = 0.95f * terminal;

    float t = 0.0f;
    float spinUp = -1.0f;
    while (t < kMaxSeconds) {
        stepShip(s, full, cfg, kStep);
        t += kStep;
        if (spinUp < 0.0f && glm::length(s.velocity) >= target) spinUp = t;
        if (spinUp >= 0.0f) break;
    }
    const float topSpeed = glm::length(s.velocity);

    // --- coast down ------------------------------------------------------
    const ShipControls idle;
    const float stopped = kStoppedFraction * terminal;
    float coast = -1.0f;
    t = 0.0f;
    while (t < kMaxSeconds) {
        stepShip(s, idle, cfg, kStep);
        t += kStep;
        if (glm::length(s.velocity) <= stopped) {
            coast = t;
            break;
        }
    }

    // --- one mouse swipe -------------------------------------------------
    //
    // The swipe is delivered in a single step, exactly as main.cpp delivers a
    // frame's accumulated motion, and the ship is then left to coast to a stop.
    // The total angle is what the player perceives as the turn.
    ShipState turner;
    ShipControls swipe;
    swipe.look = glm::vec2(kSwipePixels, 0.0f);
    const glm::quat before = turner.orientation;

    stepShip(turner, swipe, cfg, kStep);
    t = 0.0f;
    while (t < kMaxSeconds && glm::length(turner.angularVelocity()) > 1e-4f) {
        stepShip(turner, idle, cfg, kStep);
        t += kStep;
    }
    const glm::quat delta = glm::normalize(glm::inverse(before) * turner.orientation);
    const float turnDegrees = glm::degrees(2.0f * std::acos(std::min(1.0f, std::abs(delta.w))));

    // --- report ----------------------------------------------------------
    log::info("flight test, {} Hz fixed step:", 1.0f / kStep);
    log::info("  top speed          {:.2f} units/s (terminal {:.2f}, cap {:.1f})", topSpeed,
              cfg.forwardThrust / cfg.linearDamping, cfg.maxSpeed);
    log::info("  spin-up to 95%     {:.2f} s   (want 0.7 - 1.4)", spinUp);
    log::info("  coast to {:.0f}%       {:.2f} s   (want 0.7 - 1.4)", kStoppedFraction * 100.0f,
              coast);
    log::info("  {:.0f} px mouse swipe {:.1f} deg  (want 150 - 210)", kSwipePixels, turnDegrees);

    if (spinUp < 0.0f || !inRange(spinUp, 0.7f, 1.4f)) {
        log::error("spin-up time {:.2f}s is outside the tuned range", spinUp);
        ok = false;
    }
    if (coast < 0.0f || !inRange(coast, 0.7f, 1.4f)) {
        log::error("coast-down time {:.2f}s is outside the tuned range", coast);
        ok = false;
    }
    if (!inRange(turnDegrees, 150.0f, 210.0f)) {
        log::error("a full swipe turns {:.1f} degrees, not about 180", turnDegrees);
        ok = false;
    }

    log::info("flight test: {}", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace dvk::sim
