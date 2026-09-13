#include "render/mesh.h"

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

Mesh makeCube() {
    const glm::vec3 n[6] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    // Corner offsets per face, counter-clockwise when seen from outside.
    const glm::vec3 corners[6][4] = {
        {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},        // +Z
        {{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}},    // -Z
        {{1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}},        // +X
        {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}},    // -X
        {{-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1}},        // +Y
        {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}},    // -Y
    };
    const glm::vec2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

    std::vector<Vertex> vertices;
    std::vector<GLuint> indices;
    vertices.reserve(24);
    indices.reserve(36);

    for (int f = 0; f < 6; ++f) {
        const auto base = static_cast<GLuint>(vertices.size());
        for (int c = 0; c < 4; ++c) {
            vertices.push_back({corners[f][c] * 0.5f, n[f], uv[c]});
        }
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return Mesh(vertices, indices);
}

} // namespace dvk::gl
