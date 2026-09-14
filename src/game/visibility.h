#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <vector>

namespace dvk {

struct Level;

// Portal-traversal visibility: the second of the project's permanent bets.
//
// Starting in the segment the camera is in, recurse through portals, projecting
// each one to a screen-space rectangle and intersecting it with the rectangle
// it was seen through. A portal whose rectangle is empty is not visible, and
// nothing beyond it is either. This is exact enough to be worth having and
// costs a few dozen matrix multiplies per frame.
//
// No allocation once warmed: the buffers are members and are reused.
class PortalWalk {
public:
    // Recursion cap. A mine is small; this only matters for a pathological
    // level, or for looking down a corridor of identical segments.
    static constexpr int kMaxDepth = 24;

    void build(const Level& level, int fromSegment, const glm::mat4& viewProj,
               const glm::vec3& eye);

    const std::vector<int>& visible() const { return visible_; }
    bool isVisible(int segment) const {
        const auto i = static_cast<size_t>(segment);
        return i < mark_.size() && mark_[i] == frame_;
    }

    int segmentsVisited() const { return visited_; }
    int portalsCrossed() const { return crossed_; }
    int deepest() const { return deepest_; }

private:
    struct Rect {
        float minX = -1.0f, minY = -1.0f, maxX = 1.0f, maxY = 1.0f;
        bool empty() const { return minX >= maxX || minY >= maxY; }
    };

    void visit(const Level& level, int segment, const Rect& window, int depth);

    const glm::mat4* viewProj_ = nullptr;
    glm::vec3 eye_{0.0f};

    std::vector<int> visible_;
    std::vector<uint32_t> mark_;   // per segment, the frame it was last added
    std::vector<int> bestDepth_;   // shallowest depth it has been reached at
    uint32_t frame_ = 0;
    int visited_ = 0;
    int crossed_ = 0;
    int deepest_ = 0;
};

} // namespace dvk
