#pragma once

namespace dvk {

struct Level;

// Resolves every level object's collision proxy from the model it names.
//
// This is the seam between the assets and the simulation, and it lives in the
// game layer because it is the only layer allowed to look at both: it reads
// OBJ bounds through `render/model_data.h` and writes `sim::PlacedProxy` values
// into the level, so that `sim` gets a shape without ever including anything
// from `render`.
//
// Called from `loadLevel`, not from each caller. Eleven headless tests, the
// asset validator and the game itself all load levels, and a proxy table that
// one of them can forget to build is a proxy table that is empty in exactly the
// test meant to catch it being empty.
//
// Every object gets one, including the entries whose type names a robot. Those
// are never read - `initObjects` marks a robot's entry dead so the robot list
// owns it, and a robot's body carries its own authored radius rather than its
// model's bounds - but resolving them anyway keeps this function independent of
// the robot config, which `loadLevel` does not have and should not need.
void resolveObjectProxies(Level& level);

} // namespace dvk
