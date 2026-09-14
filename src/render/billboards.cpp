#include "render/billboards.h"

namespace dvk::gl {

Billboards::Billboards() {
    glCreateVertexArrays(1, &vao_);
    glCreateBuffers(1, &vbo_);
    glVertexArrayVertexBuffer(vao_, 0, vbo_, 0, sizeof(Vertex));

    glEnableVertexArrayAttrib(vao_, 0);
    glVertexArrayAttribFormat(vao_, 0, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, position));
    glVertexArrayAttribBinding(vao_, 0, 0);

    glEnableVertexArrayAttrib(vao_, 1);
    glVertexArrayAttribFormat(vao_, 1, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, color));
    glVertexArrayAttribBinding(vao_, 1, 0);

    glEnableVertexArrayAttrib(vao_, 2);
    glVertexArrayAttribFormat(vao_, 2, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, u));
    glVertexArrayAttribBinding(vao_, 2, 0);
}

Billboards::~Billboards() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void Billboards::add(const glm::vec3& center, const glm::vec3& right, const glm::vec3& up,
                     float size, const glm::vec3& color) {
    const glm::vec3 r = right * size;
    const glm::vec3 u = up * size;

    const Vertex bl{center - r - u, color, 0.0f, 0.0f};
    const Vertex br{center + r - u, color, 1.0f, 0.0f};
    const Vertex tr{center + r + u, color, 1.0f, 1.0f};
    const Vertex tl{center - r + u, color, 0.0f, 1.0f};

    vertices_.push_back(bl);
    vertices_.push_back(br);
    vertices_.push_back(tr);
    vertices_.push_back(bl);
    vertices_.push_back(tr);
    vertices_.push_back(tl);
}

void Billboards::upload() {
    if (vertices_.empty()) return;
    const size_t bytes = vertices_.size() * sizeof(Vertex);
    if (bytes > capacity_) {
        capacity_ = bytes + bytes / 2;
        glNamedBufferData(vbo_, static_cast<GLsizeiptr>(capacity_), nullptr, GL_STREAM_DRAW);
    }
    glNamedBufferSubData(vbo_, 0, static_cast<GLsizeiptr>(bytes), vertices_.data());
}

void Billboards::draw() const {
    if (vertices_.empty()) return;
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices_.size()));
}

} // namespace dvk::gl
