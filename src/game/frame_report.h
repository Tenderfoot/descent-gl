#pragma once

#include <cstdint>

namespace dvk {

namespace audio {
class Audio;
}

namespace gl {
class GpuTimers;
}

// Heap allocations made by the sim step and the draw path, counted per frame.
//
// CLAUDE.md says neither of them reaches the heap after start-up. M8 says to
// instrument that rather than believe it, so this is the instrument's tally: a
// non-zero number in the line it prints is a real finding, not noise.
//
// The two are recorded separately because they are measured at different points
// in the frame - the sim scope closes before the render frame is prepared, the
// draw scope opens after it - and a tally that took them together would not be
// able to say which one grew.
struct AllocationTally {
    // The first frames are exempt: pools and vectors reserve on their way to
    // steady state, and counting that warm-up would report a leak that is
    // really a start-up cost.
    static constexpr int kWarmupFrames = 30;

    uint64_t worstSim = 0;
    uint64_t worstDraw = 0;
    int simFrames = 0;
    int drawFrames = 0;
    int firstSimFrame = -1;
    int firstDrawFrame = -1;

    void recordSim(int frame, uint64_t allocations);
    void recordDraw(int frame, uint64_t allocations);

    // Measured, not asserted. A non-zero number means something in the sim step
    // or the draw path reached the heap on a frame after warm-up, which
    // CLAUDE.md says does not happen.
    void report(int frames) const;
};

// The step rate is the property worth checking: steps over elapsed must sit on
// the target regardless of how fast frames were drawn.
void reportStepRate(int frames, long long steps, double seconds, double targetHz);

// Frames dropped while the window was away, which are excluded from the rate
// above and so have to be accounted for somewhere.
void reportMinimized(int frames, double seconds);

// What was actually heard. Sound is the one subsystem a screenshot cannot show,
// so a run reports it rather than leaving "the guns make a noise" as a claim
// about the code. A device of "none" is the normal answer in a container and is
// not a failure; the cue counts are produced either way, because they come from
// the simulation and not from the device.
void reportSound(const audio::Audio& audio, bool disabled);

// The last frame's GPU cost, so a headless run reports it the way every other
// number in this project is reported rather than only existing behind a panel
// nobody can open without a display.
void reportGpu(const gl::GpuTimers& timers);

} // namespace dvk
