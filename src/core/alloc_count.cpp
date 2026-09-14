#include "core/alloc_count.h"

#include <cstdlib>
#include <new>

namespace dvk::alloc {
uint64_t g_allocations = 0;
uint64_t g_bytes = 0;
} // namespace dvk::alloc

// Replacing global operator new is a big hammer for a small question, but it is
// the only one that answers it honestly: it sees allocations from anywhere,
// including inside the standard library and inside a container that grew when
// nobody expected it to. A counter threaded through the project's own
// allocations would only ever find the ones already known about.
//
// The cost is two increments and a malloc, on a path that is supposed to be
// empty during a frame anyway. If it ever shows up in a profile, that is itself
// the finding.

void* operator new(size_t size) {
    ++dvk::alloc::g_allocations;
    dvk::alloc::g_bytes += size;
    if (size == 0) size = 1;
    if (void* p = std::malloc(size)) return p;
    throw std::bad_alloc();
}

void* operator new[](size_t size) { return operator new(size); }

void* operator new(size_t size, const std::nothrow_t&) noexcept {
    ++dvk::alloc::g_allocations;
    dvk::alloc::g_bytes += size;
    if (size == 0) size = 1;
    return std::malloc(size);
}

void* operator new[](size_t size, const std::nothrow_t& tag) noexcept {
    return operator new(size, tag);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
