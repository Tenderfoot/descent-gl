#include "render/model.h"

#include "core/log.h"

#include <tiny_obj_loader.h>

#include <glm/geometric.hpp>

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

} // namespace dvk::gl
