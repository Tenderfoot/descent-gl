#pragma once

#include "render/gl.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <vector>

namespace dvk::gl {

struct Vertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec2 uv;
};

// Indexed triangle mesh in a single VAO. Attribute locations match
// the lit shaders: 0 = position, 1 = normal, 2 = uv. Level geometry carries a
// fourth attribute and lives in LevelMesh instead.
class Mesh {
public:
    Mesh(const std::vector<Vertex>& vertices, const std::vector<GLuint>& indices);
    ~Mesh();
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    // Movable so factory functions can return one by value; the moved-from
    // mesh gives up its GL handles rather than deleting them twice.
    Mesh(Mesh&& other) noexcept;
    Mesh& operator=(Mesh&& other) noexcept;

    void draw() const;

private:
    GLuint vao_ = 0, vbo_ = 0, ebo_ = 0;
    GLsizei indexCount_ = 0;
};

// A unit cube centred on the origin, 24 vertices (per-face normals and UVs),
// 12 triangles, closed.
Mesh makeCube();

} // namespace dvk::gl
