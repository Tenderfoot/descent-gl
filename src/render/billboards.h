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

    // Every quad the unlit pass can ever be asked for, up front. Without it the
    // buffer grows the first time a frame is busier than any before it - which
    // is a heap allocation in the draw path, and the frame report says so.
    void reserveQuads(size_t quads) { vertices_.reserve(quads * 6); }

    void clear() { vertices_.clear(); }
    bool empty() const { return vertices_.empty(); }

    // `right` and `up` come from the view matrix, so every quad faces the
    // camera without any per-sprite maths.
    void add(const glm::vec3& center, const glm::vec3& right, const glm::vec3& up, float size,
             const glm::vec3& color);

    // The same quad, but painted from a layer of the sprite array bound to the
    // pass rather than from the shader's radial falloff. The layer rides in the
    // vertex so textured and untextured quads still share one buffer and one
    // draw; `kNoTexture` is what add() writes, and the shader branches on it.
    void addSprite(const glm::vec3& center, const glm::vec3& right, const glm::vec3& up,
                   float size, const glm::vec3& tint, int layer);

    static constexpr float kNoTexture = -1.0f;

    // A thin glowing ring whose bright edge sits `radius` from the centre -
    // an explosion's shockwave, drawn out to the edge of the blast. Another
    // negative sentinel layer, so it shares the buffer and the draw too.
    void addRing(const glm::vec3& center, const glm::vec3& right, const glm::vec3& up,
                 float radius, const glm::vec3& color);

    static constexpr float kRing = -2.0f;
    // Where the ring's bright band peaks, as a fraction of the quad's half
    // size. billboard.frag uses the same number.
    static constexpr float kRingPeak = 0.96f;

    // A beam between two points: a quad turned about its own axis to face the
    // viewer. Same vertex format and same buffer as the sprites above, so a
    // corridor full of tracers still costs the unlit pass one draw.
    void addBeam(const glm::vec3& start, const glm::vec3& end, const glm::vec3& eye,
                 const glm::vec3& cameraRight, float width, const glm::vec3& color);

    void upload();
    void draw() const;

    int count() const { return static_cast<int>(vertices_.size() / 6); }

private:
    struct Vertex {
        glm::vec3 position;
        glm::vec3 color;
        float u, v;
        float layer;
    };

    void quad(const glm::vec3& center, const glm::vec3& right, const glm::vec3& up, float size,
              const glm::vec3& color, float layer);

    std::vector<Vertex> vertices_;
    GLuint vao_ = 0, vbo_ = 0;
    size_t capacity_ = 0;
};

} // namespace dvk::gl
