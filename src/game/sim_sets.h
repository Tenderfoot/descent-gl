#pragma once

namespace dvk {

struct Level;
struct SimState;

namespace sim {
struct BodySet;
struct OccluderSet;
}

// The two per-step sets that are gathered from everything in the mine at once.
//
// Both are rebuilt from scratch every step into fixed-capacity storage the
// caller owns, because the sim step does not allocate. They are gathered rather
// than kept because the membership changes constantly - a robot dies, a crate
// is shot away, a flare burns out - and a set that was maintained incrementally
// would be one more thing that can disagree with the world.

// What stands between a light and a surface, or an eye and its target, and is
// not the mine.
//
// Built before the robots look, because their sight ray is the first thing that
// consumes it. Robots and props go in as spheres of their own body radius
// rather than their model bounds, so what blocks a line of sight is the same
// size as what stops a bullet. Pillars and crates go in on the same `alive`
// flags that decide whether they stop a bullet: shoot a crate away and it stops
// casting too.
void buildOccluders(const Level& level, const SimState& state, sim::OccluderSet& occluders);

// Everything the body-to-body pass has to keep apart: the ship, every living
// robot and every floating light.
//
// Gathered after each system has integrated and put its own bodies back inside
// the mine. This is the only place that sees all three at once, which is what
// body-to-body contact needs - it wants both masses, and it has to interleave
// with the wall resolve rather than run before or after it.
//
// Takes the state by non-const reference because the set holds pointers into
// it: the resolve moves these bodies.
void buildBodySet(SimState& state, sim::BodySet& bodies);

} // namespace dvk
