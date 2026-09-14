#pragma once

namespace dvk {

// Loads a level, runs the portal walk from every segment, and reports what the
// structural systems actually do - with no window, no GL and nobody flying.
//
// Checks, each of which fails the process:
//   * the loader's validator accepts the file
//   * every light's segment set contains the segment the light is in
//   * every segment can see itself, and no walk exceeds the segment count
//   * portal culling is doing something: at least one segment must see fewer
//     than all of them, or the cull is silently a no-op
//
// It also prints the visible and lit counts from three positions, which is the
// number M2 is supposed to be judged on and is otherwise a thing you can only
// get by flying there and reading an overlay.
int runWorldTest(const char* levelPath);

} // namespace dvk
