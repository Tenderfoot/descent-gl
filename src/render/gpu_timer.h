#pragma once

#include "render/gl.h"

#include <array>
#include <cstddef>

namespace dvk::gl {

// GPU timing with GL_TIMESTAMP queries.
//
// Every region is a pair of glQueryCounter calls, and the results are read back
// two frames later. Reading them in the frame they were issued would stall the
// pipeline waiting for the GPU to catch up, which makes the thing being measured
// slower than it is - the classic way to profile a renderer into a shape that
// does not exist.
//
// So there are three sets of queries in a ring: one being written this frame,
// and two old enough that GL_QUERY_RESULT_AVAILABLE is true without waiting. A
// frame whose results are not ready is skipped rather than waited on, and the
// last good numbers stay on screen.
class GpuTimers {
public:
    enum Pass { Prepass, Unlit, PassCount };

    // Per-light timings stop at this many lights. Beyond it the passes still
    // run and are still counted in the totals; they just are not broken out.
    static constexpr int kMaxLights = 16;

    GpuTimers();
    ~GpuTimers();
    GpuTimers(const GpuTimers&) = delete;
    GpuTimers& operator=(const GpuTimers&) = delete;

    void beginFrame();
    void endFrame();

    void passBegin(Pass p);
    void passEnd(Pass p);

    // `index` is the light's slot in the frame's active light list.
    void stencilBegin(int index);
    void stencilEnd(int index);
    void lightBegin(int index);
    void lightEnd(int index);

    bool valid() const { return valid_; }
    float pass(Pass p) const { return ms_[static_cast<size_t>(p)]; }
    float stencil(int index) const;
    float lighting(int index) const;
    int lightCount() const { return resolvedLights_; }

    // Everything the per-light passes cost, whether or not it was broken out.
    float stencilTotal() const { return stencilTotal_; }
    float lightingTotal() const { return lightingTotal_; }
    // The light that cost the most, stencil plus lighting, and what it cost.
    int worstLight() const { return worstLight_; }
    float worstLightMs() const { return worstMs_; }

private:
    static constexpr int kFrames = 3;
    // Two timestamps per region: PassCount passes, plus a stencil and a
    // lighting region for each broken-out light.
    static constexpr int kQueriesPerFrame = (PassCount + kMaxLights * 2) * 2;

    int slotFor(int region, bool end) const { return region * 2 + (end ? 1 : 0); }
    void issue(int region, bool end);
    void resolve(int frame);

    std::array<std::array<GLuint, kQueriesPerFrame>, kFrames> queries_{};
    std::array<std::array<bool, kQueriesPerFrame>, kFrames> issued_{};
    std::array<int, kFrames> lightsThisFrame_{};

    int frame_ = 0;
    int lightsSeen_ = 0;
    bool valid_ = false;

    std::array<float, PassCount> ms_{};
    std::array<float, kMaxLights> stencilMs_{};
    std::array<float, kMaxLights> lightingMs_{};
    float stencilTotal_ = 0.0f;
    float lightingTotal_ = 0.0f;
    int resolvedLights_ = 0;
    int worstLight_ = -1;
    float worstMs_ = 0.0f;
};

} // namespace dvk::gl
