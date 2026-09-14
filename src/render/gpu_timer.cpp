#include "render/gpu_timer.h"

#include <algorithm>

namespace dvk::gl {

namespace {
// A region index for the per-light slots, laid out after the fixed passes:
// light 0 stencil, light 0 lighting, light 1 stencil, ...
int stencilRegion(int index) { return GpuTimers::PassCount + index * 2; }
int lightingRegion(int index) { return GpuTimers::PassCount + index * 2 + 1; }
} // namespace

GpuTimers::GpuTimers() {
    for (auto& frame : queries_) {
        glCreateQueries(GL_TIMESTAMP, static_cast<GLsizei>(frame.size()), frame.data());
    }
}

GpuTimers::~GpuTimers() {
    for (auto& frame : queries_) {
        glDeleteQueries(static_cast<GLsizei>(frame.size()), frame.data());
    }
}

void GpuTimers::issue(int region, bool end) {
    const int slot = slotFor(region, end);
    if (slot < 0 || slot >= kQueriesPerFrame) return;
    glQueryCounter(queries_[static_cast<size_t>(frame_)][static_cast<size_t>(slot)], GL_TIMESTAMP);
    issued_[static_cast<size_t>(frame_)][static_cast<size_t>(slot)] = true;
}

void GpuTimers::beginFrame() {
    issued_[static_cast<size_t>(frame_)].fill(false);
    lightsThisFrame_[static_cast<size_t>(frame_)] = 0;
    timedLightsThisFrame_[static_cast<size_t>(frame_)] = 0;
    lightsSeen_ = 0;
    timedLightsSeen_ = 0;
}

void GpuTimers::passBegin(Pass p) { issue(static_cast<int>(p), false); }
void GpuTimers::passEnd(Pass p) { issue(static_cast<int>(p), true); }

void GpuTimers::stencilBegin(int index) {
    ++lightsSeen_;
    if (index < 0 || index >= kMaxLights) return;
    timedLightsSeen_ = std::max(timedLightsSeen_, index + 1);
    issue(stencilRegion(index), false);
}
void GpuTimers::stencilEnd(int index) {
    if (index < 0 || index >= kMaxLights) return;
    issue(stencilRegion(index), true);
}
void GpuTimers::lightBegin(int index) {
    if (index < 0 || index >= kMaxLights) return;
    issue(lightingRegion(index), false);
}
void GpuTimers::lightEnd(int index) {
    if (index < 0 || index >= kMaxLights) return;
    issue(lightingRegion(index), true);
}

void GpuTimers::endFrame() {
    lightsThisFrame_[static_cast<size_t>(frame_)] = lightsSeen_;
    timedLightsThisFrame_[static_cast<size_t>(frame_)] = timedLightsSeen_;
    frame_ = (frame_ + 1) % kFrames;
    // The frame about to be overwritten is the oldest, so its results are the
    // ones most likely to be ready. Read them before reusing its queries.
    resolve(frame_);
}

void GpuTimers::resolve(int frame) {
    const auto& q = queries_[static_cast<size_t>(frame)];
    const auto& used = issued_[static_cast<size_t>(frame)];

    // Resolve a frame as one snapshot. Mixing ready regions from this frame
    // with retained regions from an older frame makes both the total and the
    // per-light comparison meaningless.
    for (int region = 0; region < kQueriesPerFrame / 2; ++region) {
        const int b = region * 2;
        const int e = b + 1;
        if (!used[static_cast<size_t>(b)] || !used[static_cast<size_t>(e)]) continue;
        GLint ready = 0;
        glGetQueryObjectiv(q[static_cast<size_t>(e)], GL_QUERY_RESULT_AVAILABLE, &ready);
        if (!ready) return;
    }

    const auto read = [&](int region, float& out) {
        const int b = region * 2;
        const int e = b + 1;
        if (!used[static_cast<size_t>(b)] || !used[static_cast<size_t>(e)]) {
            out = 0.0f;
            return false;
        }

        GLuint64 t0 = 0, t1 = 0;
        glGetQueryObjectui64v(q[static_cast<size_t>(b)], GL_QUERY_RESULT, &t0);
        glGetQueryObjectui64v(q[static_cast<size_t>(e)], GL_QUERY_RESULT, &t1);
        out = t1 > t0 ? static_cast<float>(t1 - t0) * 1.0e-6f : 0.0f;
        return true;
    };

    bool any = false;
    for (int p = 0; p < PassCount; ++p) {
        any |= read(p, ms_[static_cast<size_t>(p)]);
    }

    stencilTotal_ = 0.0f;
    lightingTotal_ = 0.0f;
    worstLight_ = -1;
    worstMs_ = 0.0f;
    resolvedLights_ = lightsThisFrame_[static_cast<size_t>(frame)];
    resolvedTimedLights_ = timedLightsThisFrame_[static_cast<size_t>(frame)];

    for (int i = 0; i < kMaxLights; ++i) {
        float& s = stencilMs_[static_cast<size_t>(i)];
        float& l = lightingMs_[static_cast<size_t>(i)];
        const bool gotS = read(stencilRegion(i), s);
        const bool gotL = read(lightingRegion(i), l);
        stencilTotal_ += s;
        lightingTotal_ += l;
        if ((gotS || gotL) && s + l > worstMs_) {
            worstMs_ = s + l;
            worstLight_ = i;
        }
        any |= gotS || gotL;
    }

    if (any) valid_ = true;
}

float GpuTimers::stencil(int index) const {
    if (index < 0 || index >= kMaxLights) return 0.0f;
    return stencilMs_[static_cast<size_t>(index)];
}

float GpuTimers::lighting(int index) const {
    if (index < 0 || index >= kMaxLights) return 0.0f;
    return lightingMs_[static_cast<size_t>(index)];
}

} // namespace dvk::gl
