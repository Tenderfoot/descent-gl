#include "render/shadow.h"

#include "core/log.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace dvk::gl {

namespace {

// Welds positions that are the same to within a hair. A hand-authored OBJ can
// list the same corner twice, and two triangles that visually share an edge but
// hold different indices for it look exactly like an open mesh to the closure
// check below.
constexpr float kWeldEpsilon = 1e-4f;

struct WeldKey {
    int64_t x, y, z;
    bool operator==(const WeldKey& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct WeldHash {
    size_t operator()(const WeldKey& k) const {
        // Cheap mix; the tables here hold tens of entries, not millions.
        const auto h = static_cast<uint64_t>(k.x) * 73856093u ^
                       static_cast<uint64_t>(k.y) * 19349663u ^
                       static_cast<uint64_t>(k.z) * 83492791u;
        return static_cast<size_t>(h);
    }
};

WeldKey weldKey(const glm::vec3& p) {
    const auto q = [](float v) {
        return static_cast<int64_t>(std::llround(static_cast<double>(v) / kWeldEpsilon));
    };
    return {q(p.x), q(p.y), q(p.z)};
}

glm::vec4 planeOf(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 raw = glm::cross(b - a, c - a);
    const float len = glm::length(raw);
    const glm::vec3 n = len > 0.0f ? raw / len : glm::vec3(0.0f, 1.0f, 0.0f);
    return {n.x, n.y, n.z, -glm::dot(n, a)};
}

} // namespace

OccluderMesh buildOccluder(const std::vector<glm::vec3>& positions,
                           const std::vector<glm::uvec3>& triangles,
                           const std::vector<int>& triangleSegment, const std::string& name) {
    OccluderMesh mesh;

    // --- weld ------------------------------------------------------------
    std::unordered_map<WeldKey, uint32_t, WeldHash> unique;
    std::vector<uint32_t> remap(positions.size(), 0);
    unique.reserve(positions.size());

    for (size_t i = 0; i < positions.size(); ++i) {
        const WeldKey key = weldKey(positions[i]);
        const auto [it, inserted] = unique.emplace(key, static_cast<uint32_t>(mesh.positions.size()));
        if (inserted) mesh.positions.push_back(positions[i]);
        remap[i] = it->second;
    }

    mesh.triangles.reserve(triangles.size());
    mesh.triangleSegment.reserve(triangles.size());
    for (size_t t = 0; t < triangles.size(); ++t) {
        const glm::uvec3 tri = triangles[t];
        const glm::uvec3 welded{remap[tri.x], remap[tri.y], remap[tri.z]};
        // A triangle whose corners weld together has no area and no silhouette.
        // Dropping it is correct; keeping it would put a junk entry in the
        // adjacency table and fail the closure check for no reason.
        if (welded.x == welded.y || welded.y == welded.z || welded.x == welded.z) continue;
        mesh.triangles.push_back(welded);
        mesh.triangleSegment.push_back(t < triangleSegment.size() ? triangleSegment[t] : -1);
    }

    // --- adjacency -------------------------------------------------------
    struct EdgeKey {
        uint32_t a, b;
        bool operator==(const EdgeKey& o) const { return a == o.a && b == o.b; }
    };
    struct EdgeHash {
        size_t operator()(const EdgeKey& k) const {
            return static_cast<size_t>(static_cast<uint64_t>(k.a) << 32 | k.b);
        }
    };

    std::unordered_map<EdgeKey, int, EdgeHash> lookup;
    lookup.reserve(mesh.triangles.size() * 3);

    for (size_t t = 0; t < mesh.triangles.size(); ++t) {
        const glm::uvec3 tri = mesh.triangles[t];
        const uint32_t corner[3] = {tri.x, tri.y, tri.z};
        for (int e = 0; e < 3; ++e) {
            const uint32_t u = corner[e];
            const uint32_t v = corner[(e + 1) % 3];
            const EdgeKey key{std::min(u, v), std::max(u, v)};

            auto it = lookup.find(key);
            if (it == lookup.end()) {
                lookup.emplace(key, static_cast<int>(mesh.edges.size()));
                mesh.edges.push_back({key.a, key.b, static_cast<int>(t), -1});
            } else {
                OccluderMesh::Edge& edge = mesh.edges[static_cast<size_t>(it->second)];
                if (edge.tri1 >= 0) {
                    const glm::vec3& p0 = mesh.positions[key.a];
                    const glm::vec3& p1 = mesh.positions[key.b];
                    log::fatal(
                        "'{}' is not a closed mesh: the edge ({:.3f} {:.3f} {:.3f}) - "
                        "({:.3f} {:.3f} {:.3f}) is shared by more than two triangles. Shadow "
                        "volumes need every edge to have exactly two.",
                        name, p0.x, p0.y, p0.z, p1.x, p1.y, p1.z);
                }
                edge.tri1 = static_cast<int>(t);
            }
        }
    }

    int open = 0;
    for (const OccluderMesh::Edge& edge : mesh.edges) {
        if (edge.tri1 >= 0) continue;
        ++open;
        if (open <= 4) {
            const glm::vec3& p0 = mesh.positions[edge.v0];
            const glm::vec3& p1 = mesh.positions[edge.v1];
            log::error("'{}' has an open edge: ({:.3f} {:.3f} {:.3f}) - ({:.3f} {:.3f} {:.3f})",
                       name, p0.x, p0.y, p0.z, p1.x, p1.y, p1.z);
        }
    }
    if (open > 0) {
        log::fatal("'{}' is not a closed mesh: {} open edge(s), listed above. Shadow volumes "
                   "need every edge to have exactly two triangles; this is an asset error, not "
                   "a renderer one.",
                   name, open);
    }

    log::info("occluder '{}': {} vertices, {} triangles, {} edges, closed", name,
              mesh.positions.size(), mesh.triangles.size(), mesh.edges.size());
    return mesh;
}

void Occluder::reset(const OccluderMesh* mesh, const glm::mat4& transform) {
    mesh_ = mesh;
    worldPos_.resize(mesh ? mesh->positions.size() : 0);
    worldPlane_.resize(mesh ? mesh->triangles.size() : 0);
    setTransform(transform);
}

void Occluder::setTransform(const glm::mat4& transform) {
    if (!mesh_) return;
    for (size_t i = 0; i < mesh_->positions.size(); ++i) {
        worldPos_[i] = glm::vec3(transform * glm::vec4(mesh_->positions[i], 1.0f));
    }
    for (size_t t = 0; t < mesh_->triangles.size(); ++t) {
        const glm::uvec3 tri = mesh_->triangles[t];
        worldPlane_[t] = planeOf(worldPos_[tri.x], worldPos_[tri.y], worldPos_[tri.z]);
    }
}

ShadowVolumes::ShadowVolumes() {
    glCreateVertexArrays(1, &vao_);
    glCreateBuffers(1, &vbo_);
    glVertexArrayVertexBuffer(vao_, 0, vbo_, 0, sizeof(glm::vec4));
    glEnableVertexArrayAttrib(vao_, 0);
    glVertexArrayAttribFormat(vao_, 0, 4, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(vao_, 0, 0);
}

ShadowVolumes::~ShadowVolumes() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void ShadowVolumes::beginFrame() {
    vertices_.clear();
    silhouetteEdges_ = 0;
}

void ShadowVolumes::beginLight() {
    lightFirst_ = static_cast<GLint>(vertices_.size());
}

ShadowVolumes::Range ShadowVolumes::endLight() {
    return {lightFirst_, static_cast<GLsizei>(vertices_.size()) - lightFirst_};
}

void ShadowVolumes::add(const Occluder& occluder, const glm::vec3& lightPos) {
    silhouetteEdges_ += appendShadowVolume(occluder, lightPos, vertices_, facesLight_);
}

int appendShadowVolume(const Occluder& occluder, const glm::vec3& lightPos,
                       std::vector<glm::vec4>& vertices, std::vector<uint8_t>& facesLight) {
    int silhouetteEdges = 0;
    const OccluderMesh* mesh = occluder.mesh();
    if (!mesh || mesh->triangles.empty()) return 0;

    const std::vector<glm::vec3>& pos = occluder.worldPositions();
    const std::vector<glm::vec4>& planes = occluder.worldPlanes();

    // Which triangles face the light. Computed once here rather than twice per
    // edge below.
    facesLight.assign(mesh->triangles.size(), 0);
    for (size_t t = 0; t < mesh->triangles.size(); ++t) {
        const glm::vec4& plane = planes[t];
        const bool faces = glm::dot(glm::vec3(plane), lightPos) + plane.w > 0.0f;
        facesLight[t] = faces ? 1u : 0u;
    }

    const auto emitFinite = [&](const glm::vec3& p) { vertices.emplace_back(p, 1.0f); };
    // A vertex at infinity carries the direction away from the light, with
    // w = 0. Under the project's infinite far plane it lands exactly on the far
    // plane instead of being clipped, which is what keeps the counts right.
    const auto emitInfinite = [&](const glm::vec3& p) {
        vertices.emplace_back(p - lightPos, 0.0f);
    };
    // --- sides: one quad per silhouette edge -----------------------------
    for (const OccluderMesh::Edge& edge : mesh->edges) {
        const bool f0 = facesLight[static_cast<size_t>(edge.tri0)] != 0;
        const bool f1 = facesLight[static_cast<size_t>(edge.tri1)] != 0;
        if (f0 == f1) continue;

        ++silhouetteEdges;

        // Take the edge as the LIGHT-FACING triangle winds it. That is what
        // makes the quad's outward normal point out of the volume, which is
        // what makes front and back faces mean what the stencil ops assume.
        const int lit = f0 ? edge.tri0 : edge.tri1;
        const glm::uvec3 tri = mesh->triangles[static_cast<size_t>(lit)];
        const uint32_t corner[3] = {tri.x, tri.y, tri.z};

        uint32_t a = edge.v0, b = edge.v1;
        for (int e = 0; e < 3; ++e) {
            const uint32_t u = corner[e];
            const uint32_t v = corner[(e + 1) % 3];
            if ((u == edge.v0 && v == edge.v1) || (u == edge.v1 && v == edge.v0)) {
                a = u;
                b = v;
                break;
            }
        }

        const glm::vec3& pa = pos[a];
        const glm::vec3& pb = pos[b];

        // Quad a -> a(inf) -> b(inf) -> b, as two triangles.
        emitFinite(pa);
        emitInfinite(pa);
        emitFinite(pb);

        emitFinite(pb);
        emitInfinite(pa);
        emitInfinite(pb);
    }

    // --- caps ------------------------------------------------------------
    //
    // Depth-fail counting is only correct for a closed volume, so the light
    // facing triangles cap the near end as they are, and the same triangles
    // reversed and pushed to infinity cap the far end.
    for (size_t t = 0; t < mesh->triangles.size(); ++t) {
        if (!facesLight[t]) continue;
        const glm::uvec3 tri = mesh->triangles[t];

        emitFinite(pos[tri.x]);
        emitFinite(pos[tri.y]);
        emitFinite(pos[tri.z]);

        emitInfinite(pos[tri.x]);
        emitInfinite(pos[tri.z]);
        emitInfinite(pos[tri.y]);
    }
    return silhouetteEdges;
}

void ShadowVolumes::upload() {
    if (vertices_.empty()) return;

    const size_t bytes = vertices_.size() * sizeof(glm::vec4);
    if (bytes > capacity_) {
        // Grow with slack so this settles after the first few frames rather
        // than reallocating whenever a light gains an edge.
        capacity_ = bytes + bytes / 2;
        glNamedBufferData(vbo_, static_cast<GLsizeiptr>(capacity_), nullptr, GL_STREAM_DRAW);
    }
    glNamedBufferSubData(vbo_, 0, static_cast<GLsizeiptr>(bytes), vertices_.data());
}

void ShadowVolumes::draw(Range range) const {
    if (range.count <= 0) return;
    glDrawArrays(GL_TRIANGLES, range.first, range.count);
}

} // namespace dvk::gl
