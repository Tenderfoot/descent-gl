#pragma once

#include "render/gl.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <string>
#include <vector>

namespace dvk::gl {

// Stencil shadow volumes: the third permanent bet, and the project's visual
// identity. Silhouette edges extruded to infinity and counted in the stencil
// buffer with depth-fail, exactly as Doom 3 and F.E.A.R. did it.
//
// Everything here is CPU work. The meshes are tiny - tens of triangles - so a
// geometry shader would cost more in setup than it saves, and the CPU path is
// the one whose correctness can be reasoned about and printed.

// A mesh in the form shadow volumes need: welded positions, triangles, per
// triangle planes, and the edge adjacency that makes silhouette extraction a
// single pass.
//
// THE MESH MUST BE CLOSED. Every edge must have exactly two triangles. An open
// edge means the extruded volume has a hole in it, the depth-fail counts come
// out wrong on the far side of that hole, and the result is a shadow that
// flickers or inverts depending on where the camera is. Since there is no
// tolerable degraded behaviour, an open edge is a fatal asset error.
struct OccluderMesh {
    struct Edge {
        uint32_t v0 = 0, v1 = 0;   // v0 < v1
        int tri0 = -1, tri1 = -1;
    };

    std::vector<glm::vec3> positions;
    std::vector<glm::uvec3> triangles;
    std::vector<Edge> edges;

    // Which segment each triangle belongs to, for the mine. -1 for a mesh that
    // is not part of the level. Retained as source metadata; these tags must
    // not be used to cut a closed occluder into an open subset.
    std::vector<int> triangleSegment;

    size_t triangleCount() const { return triangles.size(); }
};

// Welds by position, builds adjacency, and validates closure. `name` appears in
// the error message. Fatal if any edge does not have exactly two triangles;
// the message carries the offending edge's two positions, because "this mesh is
// open" is useless without knowing where.
OccluderMesh buildOccluder(const std::vector<glm::vec3>& positions,
                           const std::vector<glm::uvec3>& triangles,
                           const std::vector<int>& triangleSegment, const std::string& name);

// One placed instance: a mesh plus where it is. World positions and planes are
// cached, so a static object pays for its transform once and the ship pays once
// per frame. The world data decides which triangles face the light; the
// volume itself is emitted in the mesh's OWN space and drawn with the same
// model matrix the mesh is drawn with - see appendShadowVolume.
class Occluder {
public:
    void reset(const OccluderMesh* mesh, const glm::mat4& transform);
    void setTransform(const glm::mat4& transform);

    const OccluderMesh* mesh() const { return mesh_; }
    const glm::mat4& transform() const { return transform_; }
    const std::vector<glm::vec3>& worldPositions() const { return worldPos_; }
    const std::vector<glm::vec4>& worldPlanes() const { return worldPlane_; }

    // A world position in the mesh's own space.
    glm::vec3 toObject(const glm::vec3& world) const;

private:
    const OccluderMesh* mesh_ = nullptr;
    glm::mat4 transform_{1.0f};
    glm::mat4 inverse_{1.0f};
    std::vector<glm::vec3> worldPos_;
    std::vector<glm::vec4> worldPlane_;
};

// Pure CPU builder shared by rendering and the closure regression test.
// Always uses the entire closed occluder; light range limits receivers, not
// the surfaces needed to balance depth-fail counts behind those receivers.
//
// The vertices come out in the occluder's OWN space, not world space, and
// must be drawn with the occluder's transform() as uModel. The volume's front
// cap is the occluder's own light-facing triangles, which the depth prepass
// has already drawn as `uProj * uView * uModel * vec4(p, 1)` with that same
// matrix. Emitting the cap as the same p through the same expression makes the
// two depths bit-identical, which depth-fail needs: a cap that lands an ulp in
// front of its own surface is not counted, and the surface shadows itself in
// a fizz of scattered pixels. Transforming on the CPU and drawing with an
// identity matrix computes the same point by a different association, and the
// last bit differs - which is what the ship, the robots, the pillars and the
// door slabs all showed. The mine's transform is the identity, so for it the
// two approaches coincide and it never had the problem.
int appendShadowVolume(const Occluder& occluder, const glm::vec3& lightPos,
                       std::vector<glm::vec4>& vertices, std::vector<uint8_t>& facesLight);

// Builds every light's volumes into one CPU buffer, uploads once per frame, and
// hands back a range per light to draw. A range is a run of draws, one per
// occluder, each with the model matrix its vertices are expressed in.
class ShadowVolumes {
public:
    struct Draw {
        GLint first = 0;
        GLsizei count = 0;
        glm::mat4 model{1.0f};
    };
    struct Range {
        GLint first = 0;       // vertex range, for the counters
        GLsizei count = 0;
        int firstDraw = 0;     // draw range
        int drawCount = 0;
    };

    ShadowVolumes();
    ~ShadowVolumes();
    ShadowVolumes(const ShadowVolumes&) = delete;
    ShadowVolumes& operator=(const ShadowVolumes&) = delete;

    // Start a frame: clears the CPU buffer without releasing its capacity.
    void beginFrame();

    // Start and finish one light's volumes. Everything added between them
    // becomes one range.
    void beginLight();
    Range endLight();

    // Adds the volume cast by one complete, closed occluder.
    void add(const Occluder& occluder, const glm::vec3& lightPos);

    // Uploads everything built this frame. Call once, after the last light.
    void upload();

    void bind() const { glBindVertexArray(vao_); }

    // Draws one light's volumes. `setModel` is called with each draw's model
    // matrix before it is issued, so the caller can upload it to the bound
    // shader's uModel.
    template <class SetModel>
    void draw(Range range, SetModel&& setModel) const {
        for (int i = range.firstDraw; i < range.firstDraw + range.drawCount; ++i) {
            const Draw& d = draws_[static_cast<size_t>(i)];
            setModel(d.model);
            glDrawArrays(GL_TRIANGLES, d.first, d.count);
        }
    }

    // Counters for the debug overlay.
    int silhouetteEdges() const { return silhouetteEdges_; }
    int volumeTriangles() const { return static_cast<int>(vertices_.size() / 3); }

private:
    // w = 1 for a finite vertex, w = 0 for one extruded to infinity. The
    // infinite ones carry a DIRECTION away from the light rather than a
    // position, which the M0 infinite far plane projection maps exactly onto
    // the far plane instead of clipping.
    std::vector<glm::vec4> vertices_;
    std::vector<Draw> draws_;
    GLint lightFirst_ = 0;
    int lightFirstDraw_ = 0;
    int silhouetteEdges_ = 0;

    GLuint vao_ = 0, vbo_ = 0;
    size_t capacity_ = 0;

    // Reused across lights so the per-frame build allocates nothing once warm.
    std::vector<uint8_t> facesLight_;
};

} // namespace dvk::gl
