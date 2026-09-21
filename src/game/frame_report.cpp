#include "game/frame_report.h"

#include "audio/audio.h"
#include "core/log.h"
#include "render/gpu_timer.h"
#include "sim/events.h"

#include <algorithm>
#include <format>
#include <string>

namespace dvk {

void AllocationTally::recordSim(int frame, uint64_t allocations) {
    if (frame <= kWarmupFrames) return;
    worstSim = std::max(worstSim, allocations);
    simFrames += allocations > 0 ? 1 : 0;
    if (allocations > 0 && firstSimFrame < 0) firstSimFrame = frame;
}

void AllocationTally::recordDraw(int frame, uint64_t allocations) {
    if (frame <= kWarmupFrames) return;
    worstDraw = std::max(worstDraw, allocations);
    drawFrames += allocations > 0 ? 1 : 0;
    if (allocations > 0 && firstDrawFrame < 0) firstDrawFrame = frame;
}

void AllocationTally::report(int frames) const {
    const int measured = std::max(0, frames - kWarmupFrames);
    if (measured <= 0) return;
    log::info(
        "allocations after a {}-frame warm-up: sim step {} on {} of {} frames (worst {}, "
        "first at frame {}), draw path {} on {} of {} (worst {}, first at frame {})",
        kWarmupFrames, simFrames == 0 ? "none" : "SOME", simFrames, measured, worstSim,
        firstSimFrame, drawFrames == 0 ? "none" : "SOME", drawFrames, measured, worstDraw,
        firstDrawFrame);
}

void reportStepRate(int frames, long long steps, double seconds, double targetHz) {
    log::info("exited cleanly: {} frames, {} sim steps in {:.3f}s ({:.1f} steps/s, target {:.0f})",
              frames, steps, seconds,
              seconds > 0.0 ? static_cast<double>(steps) / seconds : 0.0, targetHz);
}

void reportMinimized(int frames, double seconds) {
    log::info("resize-test: {} frames ({:.2f}s) skipped while minimized, excluded above", frames,
              seconds);
}

void reportSound(const audio::Audio& audio, bool disabled) {
    const auto& played = audio.played();
    // Every cue that fired, with its count, rather than a total and the loudest
    // one. A cue that never fires is the interesting case - it means either the
    // level never reached that situation or nothing emits it - and a summary
    // line cannot tell those apart. This can.
    std::string cues;
    int distinct = 0;
    for (size_t i = 0; i < played.size(); ++i) {
        if (played[i] == 0) continue;
        ++distinct;
        if (!cues.empty()) cues += ", ";
        cues += std::format("{} x{}", sim::kCueNames[i], played[i]);
    }
    log::info("sound: device \"{}\"{}, {} events over {} cue(s), {} dropped",
              disabled ? "disabled" : (audio.deviceOpen() ? "open" : "none"),
              audio.muted() ? ", muted" : "", audio.totalPlayed(), distinct,
              audio.droppedEvents());
    if (distinct > 0) log::info("  {}", cues);
}

void reportGpu(const gl::GpuTimers& timers) {
    if (!timers.valid()) {
        log::info("gpu: no timestamp results resolved (too few frames, or unsupported)");
        return;
    }
    const float total = timers.pass(gl::GpuTimers::Prepass) + timers.pass(gl::GpuTimers::Lights) +
                        timers.pass(gl::GpuTimers::Emissive) + timers.pass(gl::GpuTimers::Unlit);
    log::info(
        "gpu (last frame): prepass {:.3f} ms, all lights {:.3f}, emissive {:.3f}, unlit {:.3f}, "
        "total {:.3f} ms over {} light(s); first {} slots: stencil {:.3f}, lighting {:.3f}; "
        "worst timed light {} at {:.3f} ms",
        timers.pass(gl::GpuTimers::Prepass), timers.pass(gl::GpuTimers::Lights),
        timers.pass(gl::GpuTimers::Emissive), timers.pass(gl::GpuTimers::Unlit), total,
        timers.lightCount(), timers.timedLightCount(), timers.stencilTotal(),
        timers.lightingTotal(), timers.worstLight(), timers.worstLightMs());
}

} // namespace dvk
