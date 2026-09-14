#pragma once

#include <string>

namespace dvk::sim {

// Every constant that decides how the ship feels. Loaded from
// assets/config/flight.json at startup and re-saved from the ImGui tuning
// panel, so tuning is a matter of dragging a slider and pressing Save rather
// than editing code and rebuilding.
//
// The two damping values are exponential rates in 1/seconds, not per-step
// multipliers: velocity decays as exp(-damping * dt), which makes the feel
// independent of the step rate. The time constant is 1/damping, and a decay is
// visually finished at roughly 3 time constants.
//
// Defaults are tuned so that at full throttle the ship reaches its maximum
// speed in about a second and, on release, coasts to a visual stop in about a
// second. Both fall out of linearDamping = 3.0 (tau = 0.33 s).
struct FlightConfig {
    // Linear. Thrust is an acceleration in units/s^2, applied along the ship's
    // own axes. Terminal speed under exponential drag is thrust / damping, so
    // forwardThrust is set to maxSpeed * linearDamping and the clamp below only
    // catches the diagonal case of several axes at once.
    float forwardThrust = 60.0f;
    float strafeThrust = 42.0f;
    float liftThrust = 42.0f;
    float linearDamping = 3.0f;      // 1/s
    float maxSpeed = 20.0f;          // units/s

    // Angular, all in the ship's local frame. Mouse motion is applied as an
    // angular-velocity impulse rather than a torque: the total angle a swipe
    // produces is then impulse / angularDamping and does not depend on how many
    // simulation steps the swipe happened to land in.
    float mouseSensitivity = 0.0315f;  // (rad/s) per pixel
    float rollTorque = 20.0f;          // rad/s^2
    float pitchTorque = 14.0f;         // rad/s^2, keyboard only
    float yawTorque = 14.0f;           // rad/s^2, keyboard only
    float angularDamping = 8.0f;       // 1/s
    // Ceiling on thruster-driven rotation only. Mouse aiming is applied after
    // it and is not subject to it; see stepShip for why.
    float maxAngularSpeed = 4.0f;      // rad/s

    bool invertPitch = false;

    // Collision, tunable alongside the flight feel because that is how it is
    // judged: a radius that is too large makes corridors feel tight, and scrape
    // damping decides whether grinding a wall costs you the race.
    float shipRadius = 1.6f;
    float wallScrape = 2.5f;   // 1/s, applied only while in contact
};

// Reads the config, falling back to the defaults above for any field the file
// does not carry. A missing file is not an error: the defaults are the shipped
// tuning and the file only records deviations from them.
FlightConfig loadFlightConfig(const std::string& path);

// Writes every field. Returns false and logs on failure.
bool saveFlightConfig(const std::string& path, const FlightConfig& cfg);

} // namespace dvk::sim
