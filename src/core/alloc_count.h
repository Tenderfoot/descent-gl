#pragma once

#include <cstddef>
#include <cstdint>

namespace dvk::alloc {

// A global count of heap allocations, incremented by the operator new overloads
// in alloc_count.cpp.
//
// CLAUDE.md says there is no per-frame heap allocation in the sim step or the
// draw path, and M8 says to instrument that rather than reason about it. This is
// the instrument: bracket a region, read the count before and after, and a
// non-zero difference is an allocation nobody intended.
//
// It is a plain global rather than an atomic because everything here is single
// threaded - one loop, one GL context - and making it atomic would put a locked
// instruction in every allocation in the program to measure something that
// cannot race.
extern uint64_t g_allocations;
extern uint64_t g_bytes;

// Counts allocations between construction and `since()`.
struct Scope {
    uint64_t start = g_allocations;
    uint64_t startBytes = g_bytes;

    void reset() {
        start = g_allocations;
        startBytes = g_bytes;
    }
    uint64_t count() const { return g_allocations - start; }
    uint64_t bytes() const { return g_bytes - startBytes; }
};

} // namespace dvk::alloc
