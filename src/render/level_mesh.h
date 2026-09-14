#pragma once

#include "render/gl.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <vector>

namespace dvk {
struct Level;
}

namespace dvk::gl {

// The mine's wall triangles in one buffer, grouped so that any subset of
// segments can be drawn without rebinding or re-uploading.
//
// This is what makes the per-light passes cheap: a light draws only the
// segments in its set, and the visible set intersected with it is usually a
// handful of ranges out of one VAO.
class LevelMesh {
public:
    struct Vertex {
        glm::vec3 pos;
        glm::vec3 normal;
        glm::vec2 uv;
        float layer;   // texture array layer; float because it feeds a sampler
    };

    explicit LevelMesh(const Level& level);
    ~LevelMesh();
    LevelMesh(const LevelMesh&) = delete;
    LevelMesh& operator=(const LevelMesh&) = delete;

    void bind() const { glBindVertexArray(vao_); }

    // Caller must have bound first. Draws one segment's walls.
    void drawSegment(int segment) const;

    int segmentCount() const { return static_cast<int>(first_.size()); }
    int triangleCount() const { return total_ / 3; }

private:
    GLuint vao_ = 0, vbo_ = 0;
    std::vector<GLint> first_;    // first vertex of each segment's walls
    std::vector<GLsizei> count_;  // vertex count
    int total_ = 0;
};

} // namespace dvk::gl
