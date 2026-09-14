#pragma once

namespace dvk {

// Flies the ship into every wall of the level, from every direction, at max
// speed, and checks it never gets out - with no window, no GL and nobody at the
// keyboard.
//
// This is the milestone's real verification. "I flew around for a bit and did
// not fall out" is not a check; 14 segments times 26 directions times three
// seconds of sustained full-throttle contact is.
//
// Also cross-checks the ray and sweep queries against an independent method,
// and walks every portal in both directions.
//
// Returns a process exit code: 0 if everything held.
int runCollisionTest(const char* levelPath, const char* flightConfigPath);

} // namespace dvk
