#include "game/visibility.h"

#include "game/level.h"
#include "sim/gameflow.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>

namespace dvk {

void PortalWalk::build(const Level& level, int fromSegment, const glm::mat4& viewProj,
                       const glm::vec3& eye, const sim::DoorField* doors) {
    ++frame_;
    visible_.clear();
    mark_.resize(level.segments.size(), 0);
    bestDepth_.assign(level.segments.size(), kMaxDepth + 1);
    covered_.assign(level.segments.size(), Rect{1, 1, -1, -1});
    visited_ = 0;
    crossed_ = 0;
    deepest_ = 0;

    viewProj_ = &viewProj;
    eye_ = eye;
    doors_ = doors;

    if (fromSegment < 0 || fromSegment >= level.segmentCount()) {
        // Outside the mine (or the level is empty): fall back to drawing
        // everything rather than drawing nothing, so the failure looks like a
        // performance problem and not like a black screen.
        for (int i = 0; i < level.segmentCount(); ++i) {
            visible_.push_back(i);
            mark_[static_cast<size_t>(i)] = frame_;
        }
        visited_ = level.segmentCount();
        return;
    }

    visit(level, fromSegment, Rect{}, 0);
}

void PortalWalk::visit(const Level& level, int segment, const Rect& window, int depth) {
    const auto si = static_cast<size_t>(segment);
    const Rect previous = covered_[si];
    const bool contained = !previous.empty() &&
        window.minX >= previous.minX && window.minY >= previous.minY &&
        window.maxX <= previous.maxX && window.maxY <= previous.maxY;
    if (contained && bestDepth_[si] <= depth) return;

    // A shorter route does not imply a wider view. Revisit when another
    // portal adds screen coverage, or when a shorter route restores traversal
    // budget. The bounding union may overdraw but cannot hide a visible exit.
    const Rect expanded = previous.empty() ? window : Rect{
        std::min(previous.minX, window.minX), std::min(previous.minY, window.minY),
        std::max(previous.maxX, window.maxX), std::max(previous.maxY, window.maxY)};
    covered_[si] = expanded;
    depth = std::min(depth, bestDepth_[si]);
    ++visited_;
    deepest_ = std::max(deepest_, depth);

    if (mark_[si] != frame_) {
        mark_[si] = frame_;
        visible_.push_back(segment);
    }
    bestDepth_[si] = depth;

    if (depth >= kMaxDepth) return;

    const Segment& seg = level.segments[si];
    for (int side = 0; side < SideCount; ++side) {
        const SideData& sd = seg.sides[static_cast<size_t>(side)];
        if (!sd.isPortal()) continue;
        if (doors_ && doors_->isOpaque(segment, side)) continue;

        const std::array<glm::vec3, 4> corners = level.sideCorners(segment, side);

        // Clip the portal against the near plane BEFORE projecting.
        //
        // A portal beside the camera straddles it: some corners are in front of
        // the eye and some behind. Projecting a behind-the-eye corner divides
        // by a negative w and puts it on the wrong side of the screen, so the
        // bounding rectangle comes out mirrored and enormous. Falling back to
        // the parent window in that case is conservative but useless - at a
        // segment's centre nearly every portal straddles, so every portal
        // inherited the full screen and the cull reported the whole mine
        // visible from anywhere in it.
        //
        // Sutherland-Hodgman against the single plane w > epsilon. Four corners
        // clipped by one plane give at most five, hence the fixed buffer and no
        // allocation.
        std::array<glm::vec4, 8> poly{};
        int polyCount = 0;
        {
            std::array<glm::vec4, 4> clip{};
            for (size_t i = 0; i < 4; ++i) {
                clip[i] = (*viewProj_) * glm::vec4(corners[i], 1.0f);
            }
            constexpr float kEps = 1e-4f;
            for (size_t i = 0; i < 4; ++i) {
                const glm::vec4& a = clip[i];
                const glm::vec4& b = clip[(i + 1) % 4];
                const bool aIn = a.w > kEps;
                const bool bIn = b.w > kEps;
                if (aIn) poly[static_cast<size_t>(polyCount++)] = a;
                if (aIn != bIn) {
                    const float t = (kEps - a.w) / (b.w - a.w);
                    poly[static_cast<size_t>(polyCount++)] = a + (b - a) * t;
                }
            }
        }
        if (polyCount == 0) continue;   // entirely behind the eye

        Rect portal{1.0f, 1.0f, -1.0f, -1.0f};
        for (int i = 0; i < polyCount; ++i) {
            const glm::vec4& c = poly[static_cast<size_t>(i)];
            const float x = c.x / c.w;
            const float y = c.y / c.w;
            portal.minX = std::min(portal.minX, x);
            portal.maxX = std::max(portal.maxX, x);
            portal.minY = std::min(portal.minY, y);
            portal.maxY = std::max(portal.maxY, y);
        }

        Rect next;
        next.minX = std::max(expanded.minX, portal.minX);
        next.maxX = std::min(expanded.maxX, portal.maxX);
        next.minY = std::max(expanded.minY, portal.minY);
        next.maxY = std::min(expanded.maxY, portal.maxY);
        if (next.empty()) continue;

        ++crossed_;
        visit(level, sd.neighbour, next, depth + 1);
    }
}

} // namespace dvk
