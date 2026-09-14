#pragma once

#include "render/gl.h"

#include <glm/vec3.hpp>
#include <vector>

namespace dvk::gl {

// Camera-facing additive quads: bolts, flares, impact flashes, explosions.
//
// Expanded on the CPU into one buffer and drawn in a single call, rather than
// one draw per sprite with the corners built in a shader. At a couple of
// hundred sprites the upload is cheaper than the state changes, and it keeps
// the unlit pass to one draw.
class Billboards {
public:
    Billboards();
    ~Billboards();
    Billboards(const Billboards&) = delete;
    Billboards& operator=(const Billboards&) = delete;

    void clear() { vertices_.clear(); }
    bool empty() const { return vertices_.empty(); }

    // `right` and `up` come from the view matrix, so every quad faces the
    // camera without any per-sprite maths.
    void add(const glm::vec3& center, const glm::vec3& right, const glm::vec3& up, float size,
             const glm::vec3& color);

    void upload();
    void draw() const;

    int count() const { return static_cast<int>(vertices_.size() / 6); }

private:
    struct Vertex {
        glm::vec3 position;
        glm::vec3 color;
        float u, v;
    };

    std::vector<Vertex> vertices_;
    GLuint vao_ = 0, vbo_ = 0;
    size_t capacity_ = 0;
};

} // namespace dvk::gl
