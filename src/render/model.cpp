#include "render/model.h"
#include "render/model_data.h"

#include "core/log.h"

#include <tiny_obj_loader.h>

#include <glm/common.hpp>
#include <glm/geometric.hpp>


#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace dvk::gl {

ModelData loadObj(const std::string& path) {
    tinyobj::ObjReader reader;
    tinyobj::ObjReaderConfig config;
    config.triangulate = true;
    config.vertex_color = false;

    if (!reader.ParseFromFile(path, config)) {
        log::fatal("cannot load model '{}': {}", path,
                   reader.Error().empty() ? "unknown error" : reader.Error());
    }
    if (!reader.Warning().empty()) {
        log::warn("model '{}': {}", path, reader.Warning());
    }

    const tinyobj::attrib_t& attrib = reader.GetAttrib();

    ModelData model;
    model.positions.reserve(attrib.vertices.size() / 3);
    for (size_t i = 0; i + 2 < attrib.vertices.size(); i += 3) {
        model.positions.emplace_back(attrib.vertices[i], attrib.vertices[i + 1],
                                     attrib.vertices[i + 2]);
    }

    for (const tinyobj::shape_t& shape : reader.GetShapes()) {
        const auto& indices = shape.mesh.indices;
        if (indices.size() % 3 != 0) {
            log::fatal("model '{}': shape '{}' is not triangulated", path, shape.name);
        }
        for (size_t i = 0; i < indices.size(); i += 3) {
            model.triangles.emplace_back(static_cast<unsigned>(indices[i + 0].vertex_index),
                                         static_cast<unsigned>(indices[i + 1].vertex_index),
                                         static_cast<unsigned>(indices[i + 2].vertex_index));
            for (size_t c = 0; c < 3; ++c) {
                const int uv = indices[i + c].texcoord_index;
                model.uvs.emplace_back(uv >= 0 ? attrib.texcoords[2 * static_cast<size_t>(uv) + 0] : 0.0f,
                                       uv >= 0 ? attrib.texcoords[2 * static_cast<size_t>(uv) + 1] : 0.0f);
            }
        }
    }

    log::info("model '{}' loaded: {} vertices, {} triangles", path, model.positions.size(),
              model.triangles.size());
    return model;
}

bool modelBounds(const ModelData& model, glm::vec3& lo, glm::vec3& hi) {
    if (model.positions.empty()) return false;
    glm::vec3 low = model.positions.front();
    glm::vec3 high = low;
    for (const glm::vec3& p : model.positions) {
        low = glm::min(low, p);
        high = glm::max(high, p);
    }
    lo = low;
    hi = high;
    return true;
}

namespace {

// Where a plane already in the list matches this one, within tolerance.
int findPlane(const std::vector<glm::vec4>& planes, const glm::vec3& n, float d, float tol) {
    for (size_t i = 0; i < planes.size(); ++i) {
        const glm::vec3 pn(planes[i]);
        if (glm::dot(pn, n) > 0.9999f && std::abs(planes[i].w - d) <= tol) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

bool modelConvexHull(const ModelData& model, ConvexHullFit& fit) {
    fit = ConvexHullFit{};
    if (model.positions.size() < 4 || model.triangles.size() < 4) return false;
    if (model.positions.size() > static_cast<size_t>(kMaxHullVertices)) return false;

    glm::vec3 lo(0.0f);
    glm::vec3 hi(0.0f);
    if (!modelBounds(model, lo, hi)) return false;
    const glm::vec3 size = hi - lo;
    const float extent = std::max(size.x, std::max(size.y, size.z));
    if (extent <= 1e-6f) return false;
    // Relative, so a model authored at any scale is judged the same way.
    const float tol = extent * 1e-4f;

    // A point known to be inside, used only to orient the planes. The mean of
    // the vertices is inside any convex solid, which saves this from having to
    // trust the winding - and the winding is exactly the thing an art pipeline
    // gets wrong quietly.
    glm::vec3 interior(0.0f);
    for (const glm::vec3& p : model.positions) interior += p;
    interior /= static_cast<float>(model.positions.size());

    for (const glm::uvec3& tri : model.triangles) {
        const glm::vec3& a = model.positions[tri.x];
        const glm::vec3& b = model.positions[tri.y];
        const glm::vec3& c = model.positions[tri.z];
        glm::vec3 n = glm::cross(b - a, c - a);
        const float area = glm::length(n);
        if (area <= 1e-9f) continue;   // degenerate sliver, carries no plane
        n /= area;
        float d = -glm::dot(n, a);
        if (glm::dot(n, interior) + d > 0.0f) {
            n = -n;
            d = -d;
        }
        // A flat or hollow model puts its own middle on a face. There is no
        // solid there to collide with, so say so rather than building one.
        if (glm::dot(n, interior) + d > -tol) return false;
        if (findPlane(fit.planes, n, d, tol) < 0) {
            if (fit.planes.size() >= static_cast<size_t>(kMaxHullPlanes)) return false;
            fit.planes.emplace_back(n, d);
        }
    }
    if (fit.planes.size() < 4) return false;

    // Convex exactly when no vertex escapes any face.
    for (const glm::vec4& plane : fit.planes) {
        for (const glm::vec3& p : model.positions) {
            fit.worstOutside = std::max(fit.worstOutside, glm::dot(glm::vec3(plane), p) + plane.w);
        }
    }
    if (fit.worstOutside > tol) return false;

    fit.vertices = model.positions;
    fit.lo = lo;
    fit.hi = hi;
    fit.centre = (lo + hi) * 0.5f;
    for (const glm::vec3& p : fit.vertices) {
        fit.radius = std::max(fit.radius, glm::length(p - fit.centre));
    }

    // Each face's boundary. For a convex solid the vertices ON a plane are
    // exactly that face's corners, so the boundary is recovered by sorting them
    // by angle about their own middle - no walk over triangle adjacency, and
    // nothing to get wrong at a shared edge.
    std::vector<std::pair<float, int>> ring;
    fit.faceFirst.reserve(fit.planes.size() + 1);
    fit.faceFirst.push_back(0);
    for (const glm::vec4& plane : fit.planes) {
        const glm::vec3 n(plane);
        ring.clear();

        glm::vec3 middle(0.0f);
        for (size_t vi = 0; vi < fit.vertices.size(); ++vi) {
            if (std::abs(glm::dot(n, fit.vertices[vi]) + plane.w) <= tol) {
                middle += fit.vertices[vi];
                ring.emplace_back(0.0f, static_cast<int>(vi));
            }
        }
        if (ring.size() < 3) return false;   // a plane with no face on it
        middle /= static_cast<float>(ring.size());

        // An in-plane basis. `u` points at the first corner, and `w = n x u`
        // makes (u, w, n) right-handed, so sorting by increasing angle winds
        // the face counter-clockwise seen from outside - which is what the
        // edge test in `closestPointOnConvexHull` assumes.
        glm::vec3 u = fit.vertices[static_cast<size_t>(ring.front().second)] - middle;
        const float uLength = glm::length(u);
        if (uLength <= 1e-9f) return false;
        u /= uLength;
        const glm::vec3 w = glm::cross(n, u);

        for (auto& entry : ring) {
            const glm::vec3 away = fit.vertices[static_cast<size_t>(entry.second)] - middle;
            entry.first = std::atan2(glm::dot(away, w), glm::dot(away, u));
        }
        std::sort(ring.begin(), ring.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        for (const auto& entry : ring) fit.faceVertex.push_back(entry.second);
        fit.faceFirst.push_back(static_cast<int>(fit.faceVertex.size()));
    }

    return true;
}

Mesh toMesh(const ModelData& model) {
    std::vector<Vertex> vertices;
    std::vector<GLuint> indices;
    vertices.reserve(model.triangles.size() * 3);
    indices.reserve(model.triangles.size() * 3);

    for (size_t t = 0; t < model.triangles.size(); ++t) {
        const glm::uvec3 tri = model.triangles[t];
        const glm::vec3& a = model.positions[tri.x];
        const glm::vec3& b = model.positions[tri.y];
        const glm::vec3& c = model.positions[tri.z];

        // Flat normal from the winding. A degenerate triangle would produce a
        // zero normal rather than a NaN; it draws as unlit instead of as a hole.
        const glm::vec3 raw = glm::cross(b - a, c - a);
        const float len = glm::length(raw);
        const glm::vec3 normal = len > 0.0f ? raw / len : glm::vec3(0.0f, 1.0f, 0.0f);

        for (size_t c2 = 0; c2 < 3; ++c2) {
            const auto corner = static_cast<GLuint>(vertices.size());
            vertices.push_back({model.positions[tri[static_cast<glm::length_t>(c2)]], normal,
                                model.uvs[t * 3 + c2]});
            indices.push_back(corner);
        }
    }

    return Mesh(vertices, indices);
}

ModelData cubeModel() {
    ModelData cube;
    cube.positions = {
        {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f},
        {-0.5f, -0.5f, 0.5f},  {0.5f, -0.5f, 0.5f},  {0.5f, 0.5f, 0.5f},  {-0.5f, 0.5f, 0.5f}};

    // Four corners per face, counter-clockwise seen from outside, split across
    // the c0-c2 diagonal. One table, so the drawn triangles and the cast ones
    // cannot drift apart.
    const uint32_t faces[6][4] = {
        {4, 5, 6, 7},   // +Z
        {1, 0, 3, 2},   // -Z
        {5, 1, 2, 6},   // +X
        {0, 4, 7, 3},   // -X
        {7, 6, 2, 3},   // +Y
        {0, 1, 5, 4},   // -Y
    };
    const glm::vec2 uv[4] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};

    cube.triangles.reserve(12);
    cube.uvs.reserve(36);
    for (const auto& f : faces) {
        cube.triangles.push_back({f[0], f[1], f[2]});
        cube.uvs.insert(cube.uvs.end(), {uv[0], uv[1], uv[2]});
        cube.triangles.push_back({f[0], f[2], f[3]});
        cube.uvs.insert(cube.uvs.end(), {uv[0], uv[2], uv[3]});
    }
    return cube;
}

} // namespace dvk::gl
