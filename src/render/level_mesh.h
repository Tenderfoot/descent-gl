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
        float layer;      // texture array layer; float because it feeds a sampler
        float emissive;   // SideData::emissive, 0 for an ordinary wall
    };

    explicit LevelMesh(const Level& level);
    ~LevelMesh();
    LevelMesh(const LevelMesh&) = delete;
    LevelMesh& operator=(const LevelMesh&) = delete;

    void bind() const { glBindVertexArray(vao_); }

    // Caller must have bound first. Draws one segment's walls.
    void drawSegment(int segment) const;

    // Just the sides of that segment that emit.
    //
    // A segment's sides are emitted with the dark ones first and the emitting
    // ones last, so "the ones that emit" is the tail of the same range rather
    // than a second buffer or a per-frame filter. Nothing else cares what order
    // the triangles are in - the range is drawn whole by every other pass, and
    // shadow volumes are built from Level::walls, not from here.
    void drawSegmentEmissive(int segment) const;

    // Both faces of one of Level::grates. Its segments' ranges above do not
    // include it: a grate belongs to two segments at once, so the caller
    // decides from either of them whether it is drawn.
    void drawGrate(int grate) const;
    int grateCount() const { return static_cast<int>(grateFirst_.size()); }

    int segmentCount() const { return static_cast<int>(first_.size()); }
    int triangleCount() const { return total_ / 3; }
    // Whether any side in the level emits at all, so the pass can be skipped
    // entirely on the levels that do not use it - which is all of them so far.
    bool anyEmissive() const { return emissiveTotal_ > 0; }

private:
    GLuint vao_ = 0, vbo_ = 0;
    std::vector<GLint> first_;    // first vertex of each segment's walls
    std::vector<GLsizei> count_;  // vertex count
    std::vector<GLint> emissiveFirst_;    // first emitting vertex, inside the above
    std::vector<GLsizei> emissiveCount_;
    std::vector<GLint> grateFirst_;   // first vertex of each grate, after every segment
    static constexpr GLsizei kGrateVertices = 12;   // two faces, two triangles each
    int total_ = 0;
    int emissiveTotal_ = 0;
};

} // namespace dvk::gl
