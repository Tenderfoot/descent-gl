#pragma once

namespace dvk {

// Fires every weapon, at walls and at targets, with no window and no GL, and
// checks what actually happened to the simulation. Returns a process exit code.
int runWeaponsTest(const char* levelPath, const char* weaponConfigPath);

} // namespace dvk
