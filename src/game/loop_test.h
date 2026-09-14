#pragma once

namespace dvk {

// Plays the level's win and lose conditions through, with no window and no GL:
// keys against doors, a closed door against light, the reactor, the countdown,
// the exit, and every kind of pickup. Returns a process exit code.
int runLoopTest(const char* levelPath, const char* weaponConfigPath, const char* robotConfigPath);

} // namespace dvk
