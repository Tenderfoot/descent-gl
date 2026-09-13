#pragma once

#include <chrono>

namespace dvk {

// Monotonic wall clock in seconds since construction.
class Clock {
public:
    Clock() : start_(std::chrono::steady_clock::now()) {}
    double seconds() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }

private:
    std::chrono::steady_clock::time_point start_;
};

} // namespace dvk
