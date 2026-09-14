#pragma once

namespace dvk::sim {

// Measures the flight model against the three numbers M1 is tuned to hit, with
// no window, no GL and nobody at the keyboard:
//
//   * time from a standing start at full throttle to 95% of top speed
//   * time from top speed to a visual stop once the throttle is released
//   * the rotation produced by one full mouse swipe across a pad
//
// Runs the same stepShip at the same fixed rate the game does, so what it
// reports is the real model rather than an analytic approximation of it.
// Returns a process exit code: 0 if every measurement is inside tolerance.
int runFlightTest(const char* configPath);

} // namespace dvk::sim
