#pragma once

#include <string>

namespace dvk::audio {

// Drives the whole audio path with no device, no window and no sound card:
// loads every shipped clip, then runs the mixer and checks what came out of it.
//
// Sound is the one subsystem whose output nobody can look at in a screenshot,
// so this is how a claim about it is made checkable. It measures rather than
// asserts style: attenuation over distance, the stereo image, the behaviour of
// the voice pool when it is full, whether a loop actually loops, whether the
// bus clips, and whether the render path reaches the heap.
//
// Returns a process exit code: 0 if every check passed.
int runSoundTest(const std::string& soundDir);

} // namespace dvk::audio
