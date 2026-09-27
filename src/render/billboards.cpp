#include "render/billboards.h"

#include <glm/geometric.hpp>

#include <cmath>

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

    glEnableVertexArrayAttrib(vao_, 3);
    glVertexArrayAttribFormat(vao_, 3, 1, GL_FLOAT, GL_FALSE, offsetof(Vertex, layer));
    glVertexArrayAttribBinding(vao_, 3, 0);
}

Billboards::~Billboards() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void Billboards::quad(const glm::vec3& center, const glm::vec3& right, const glm::vec3& up,
                      float size, const glm::vec3& color, float layer) {
    const glm::vec3 r = right * size;
    const glm::vec3 u = up * size;

    const Vertex bl{center - r - u, color, 0.0f, 0.0f, layer};
    const Vertex br{center + r - u, color, 1.0f, 0.0f, layer};
    const Vertex tr{center + r + u, color, 1.0f, 1.0f, layer};
    const Vertex tl{center - r + u, color, 0.0f, 1.0f, layer};

    vertices_.push_back(bl);
    vertices_.push_back(br);
    vertices_.push_back(tr);
    vertices_.push_back(bl);
    vertices_.push_back(tr);
    vertices_.push_back(tl);
}

void Billboards::add(const glm::vec3& center, const glm::vec3& right, const glm::vec3& up,
                     float size, const glm::vec3& color) {
    quad(center, right, up, size, color, kNoTexture);
}

void Billboards::addSprite(const glm::vec3& center, const glm::vec3& right, const glm::vec3& up,
                           float size, const glm::vec3& tint, int layer) {
    quad(center, right, up, size, tint, static_cast<float>(layer));
}

void Billboards::addRing(const glm::vec3& center, const glm::vec3& right, const glm::vec3& up,
                         float radius, const glm::vec3& color) {
    quad(center, right, up, radius / kRingPeak, color, kRing);
}

void Billboards::addBeam(const glm::vec3& start, const glm::vec3& end, const glm::vec3& eye,
                         const glm::vec3& cameraRight, float width, const glm::vec3& color) {
    const glm::vec3 axis = end - start;
    const float length = glm::length(axis);
    if (length < 1e-4f) return;
    const glm::vec3 dir = axis / length;

    // The beam's own axis crossed with the line of sight gives the direction to
    // widen in, so the quad turns to face the camera about the beam rather than
    // about its centre. Looking straight down a beam the cross product
    // degenerates. In that case the camera's own right vector is the stable
    // answer: the beam must remain visible rather than vanish or flip.
    const glm::vec3 toEye = eye - start;
    glm::vec3 side = glm::cross(dir, toEye);
    float sideLength = glm::length(side);
    if (sideLength < 1e-5f) {
        side = cameraRight - dir * glm::dot(cameraRight, dir);
        sideLength = glm::length(side);
        if (sideLength < 1e-5f) {
            const glm::vec3 fallback = std::abs(dir.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f)
                                                              : glm::vec3(1.0f, 0.0f, 0.0f);
            side = glm::cross(dir, fallback);
            sideLength = glm::length(side);
        }
    }
    const glm::vec3 w = side * (width / sideLength);

    const Vertex bl{start - w, color, 0.0f, 0.0f, kNoTexture};
    const Vertex br{start + w, color, 1.0f, 0.0f, kNoTexture};
    const Vertex tr{end + w, color, 1.0f, 1.0f, kNoTexture};
    const Vertex tl{end - w, color, 0.0f, 1.0f, kNoTexture};

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
