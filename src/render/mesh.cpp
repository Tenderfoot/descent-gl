#include "render/mesh.h"

#include <glm/gtc/constants.hpp>

#include <cmath>

namespace dvk::gl {

Mesh::Mesh(const std::vector<Vertex>& vertices, const std::vector<GLuint>& indices)
    : indexCount_(static_cast<GLsizei>(indices.size())) {
    glCreateVertexArrays(1, &vao_);
    glCreateBuffers(1, &vbo_);
    glCreateBuffers(1, &ebo_);

    glNamedBufferStorage(vbo_, static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)),
                         vertices.data(), 0);
    glNamedBufferStorage(ebo_, static_cast<GLsizeiptr>(indices.size() * sizeof(GLuint)),
                         indices.data(), 0);

    glVertexArrayVertexBuffer(vao_, 0, vbo_, 0, sizeof(Vertex));
    glVertexArrayElementBuffer(vao_, ebo_);

    glEnableVertexArrayAttrib(vao_, 0);
    glVertexArrayAttribFormat(vao_, 0, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, pos));
    glVertexArrayAttribBinding(vao_, 0, 0);

    glEnableVertexArrayAttrib(vao_, 1);
    glVertexArrayAttribFormat(vao_, 1, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, normal));
    glVertexArrayAttribBinding(vao_, 1, 0);

    glEnableVertexArrayAttrib(vao_, 2);
    glVertexArrayAttribFormat(vao_, 2, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, uv));
    glVertexArrayAttribBinding(vao_, 2, 0);
}

Mesh::~Mesh() {
    if (ebo_) glDeleteBuffers(1, &ebo_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

Mesh::Mesh(Mesh&& other) noexcept
    : vao_(other.vao_), vbo_(other.vbo_), ebo_(other.ebo_), indexCount_(other.indexCount_) {
    other.vao_ = other.vbo_ = other.ebo_ = 0;
    other.indexCount_ = 0;
}

Mesh& Mesh::operator=(Mesh&& other) noexcept {
    if (this != &other) {
        if (ebo_) glDeleteBuffers(1, &ebo_);
        if (vbo_) glDeleteBuffers(1, &vbo_);
        if (vao_) glDeleteVertexArrays(1, &vao_);
        vao_ = other.vao_;
        vbo_ = other.vbo_;
        ebo_ = other.ebo_;
        indexCount_ = other.indexCount_;
        other.vao_ = other.vbo_ = other.ebo_ = 0;
        other.indexCount_ = 0;
    }
    return *this;
}

void Mesh::draw() const {
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
}

Mesh sphereMesh(int slices, int stacks) {
    std::vector<Vertex> vertices;
    std::vector<GLuint> indices;
    vertices.reserve(static_cast<size_t>((slices + 1) * (stacks + 1)));
    indices.reserve(static_cast<size_t>(slices * stacks * 6));

    // Rings from the +Y pole down to the -Y pole. The seam column is
    // duplicated so the UVs can run 0..1 without wrapping.
    for (int i = 0; i <= stacks; ++i) {
        const float theta = glm::pi<float>() * static_cast<float>(i) / static_cast<float>(stacks);
        for (int j = 0; j <= slices; ++j) {
            const float phi =
                glm::two_pi<float>() * static_cast<float>(j) / static_cast<float>(slices);
            const glm::vec3 p{std::sin(theta) * std::cos(phi), std::cos(theta),
                              std::sin(theta) * std::sin(phi)};
            vertices.push_back({p, p,
                                {static_cast<float>(j) / static_cast<float>(slices),
                                 static_cast<float>(i) / static_cast<float>(stacks)}});
        }
    }

    // Counter-clockwise seen from outside. The triangles that touch a pole are
    // degenerate on one edge and rasterize nothing, which is cheaper than a
    // special case.
    const auto row = static_cast<GLuint>(slices + 1);
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            const GLuint a = static_cast<GLuint>(i) * row + static_cast<GLuint>(j);
            const GLuint b = a + row;
            indices.insert(indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    }

    return Mesh(vertices, indices);
}

} // namespace dvk::gl
