#include "render/model.h"
#include "render/model_data.h"

#include "core/log.h"

#include <tiny_obj_loader.h>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cstdint>

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
