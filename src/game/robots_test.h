#pragma once

namespace dvk {

// Drives the robot state machine through every transition the plan names, with
// no window and no GL. Returns a process exit code.
int runRobotsTest(const char* levelPath, const char* weaponConfigPath, const char* robotConfigPath);

} // namespace dvk
