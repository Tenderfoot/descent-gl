#include "render/debug_lines.h"

namespace dvk::gl {

DebugLines::DebugLines() {
    glCreateVertexArrays(1, &vao_);
    glCreateBuffers(1, &vbo_);
    glVertexArrayVertexBuffer(vao_, 0, vbo_, 0, sizeof(Vertex));

    glEnableVertexArrayAttrib(vao_, 0);
    glVertexArrayAttribFormat(vao_, 0, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, position));
    glVertexArrayAttribBinding(vao_, 0, 0);

    glEnableVertexArrayAttrib(vao_, 1);
    glVertexArrayAttribFormat(vao_, 1, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, color));
    glVertexArrayAttribBinding(vao_, 1, 0);
}

DebugLines::~DebugLines() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void DebugLines::line(const glm::vec3& a, const glm::vec3& b, const glm::vec3& color) {
    vertices_.push_back({a, color});
    vertices_.push_back({b, color});
}

void DebugLines::cross(const glm::vec3& p, float size, const glm::vec3& color) {
    line(p - glm::vec3(size, 0, 0), p + glm::vec3(size, 0, 0), color);
    line(p - glm::vec3(0, size, 0), p + glm::vec3(0, size, 0), color);
    line(p - glm::vec3(0, 0, size), p + glm::vec3(0, 0, size), color);
}

void DebugLines::upload() {
    if (vertices_.empty()) return;
    const size_t bytes = vertices_.size() * sizeof(Vertex);
    if (bytes > capacity_) {
        capacity_ = bytes + bytes / 2;
        glNamedBufferData(vbo_, static_cast<GLsizeiptr>(capacity_), nullptr, GL_STREAM_DRAW);
    }
    glNamedBufferSubData(vbo_, 0, static_cast<GLsizeiptr>(bytes), vertices_.data());
}

void DebugLines::draw() const {
    if (vertices_.empty()) return;
    glBindVertexArray(vao_);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(vertices_.size()));
}

} // namespace dvk::gl
