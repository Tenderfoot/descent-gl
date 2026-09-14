#pragma once

#include "render/gl.h"

#include <glm/vec3.hpp>
#include <vector>

namespace dvk::gl {

// A per-frame line list, for the things that are easier to look at than to
// reason about: wall normals, the contact points collision resolved against,
// where a ray actually hit.
//
// Rebuilt from scratch each frame and uploaded once. The vector keeps its
// capacity, so after the first few frames this stops allocating.
class DebugLines {
public:
    DebugLines();
    ~DebugLines();
    DebugLines(const DebugLines&) = delete;
    DebugLines& operator=(const DebugLines&) = delete;

    void clear() { vertices_.clear(); }
    bool empty() const { return vertices_.empty(); }

    void line(const glm::vec3& a, const glm::vec3& b, const glm::vec3& color);
    // Three axis-aligned bars through a point, for marking a position.
    void cross(const glm::vec3& p, float size, const glm::vec3& color);

    void upload();
    void draw() const;

private:
    struct Vertex {
        glm::vec3 position;
        glm::vec3 color;
    };

    std::vector<Vertex> vertices_;
    GLuint vao_ = 0, vbo_ = 0;
    size_t capacity_ = 0;
};

} // namespace dvk::gl
