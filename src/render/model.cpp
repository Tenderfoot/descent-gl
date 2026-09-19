#include "render/model.h"
#include "render/model_data.h"

#include "core/log.h"

#include <tiny_obj_loader.h>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <glm/gtc/constants.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
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

bool modelTaperedCylinder(const ModelData& model, TaperedCylinderFit& fit) {
    glm::vec3 lo(0.0f);
    glm::vec3 hi(0.0f);
    if (!modelBounds(model, lo, hi)) return false;

    const float height = hi.y - lo.y;
    const float width = std::max(hi.x - lo.x, hi.z - lo.z);
    if (height <= 1e-4f || width <= 1e-4f) return false;

    // Group the vertices into rings by height. The tolerance is relative, so a
    // model authored in any scale buckets the same way.
    const float bucket = std::max(1e-4f, height * 1e-3f);
    std::vector<float> levels;
    for (const glm::vec3& p : model.positions) {
        bool found = false;
        for (const float y : levels) {
            if (std::abs(p.y - y) <= bucket) {
                found = true;
                break;
            }
        }
        if (!found) levels.push_back(p.y);
    }
    if (levels.size() < 2 || levels.size() > 64) return false;
    std::sort(levels.begin(), levels.end());

    // The axis, from the rings themselves rather than from the bounding box: a
    // regular ring's centroid is its centre exactly, and a model whose rings do
    // not share one axis fails the radius check below rather than being fitted
    // to a lie.
    glm::vec2 axis(0.0f);
    for (const float y : levels) {
        glm::vec2 sum(0.0f);
        int members = 0;
        for (const glm::vec3& p : model.positions) {
            if (std::abs(p.y - y) > bucket) continue;
            sum += glm::vec2(p.x, p.z);
            ++members;
        }
        if (members == 0) return false;
        axis += sum / static_cast<float>(members);
    }
    axis /= static_cast<float>(levels.size());

    std::vector<float> radii;
    radii.reserve(levels.size());
    fit.sides = 0;
    fit.roundness = 1.0f;

    std::vector<float> angles;
    for (const float y : levels) {
        angles.clear();
        float ringRadius = 0.0f;
        float tightest = std::numeric_limits<float>::max();
        for (const glm::vec3& p : model.positions) {
            if (std::abs(p.y - y) > bucket) continue;
            const glm::vec2 flat = glm::vec2(p.x, p.z) - axis;
            const float r = glm::length(flat);
            // A cap fan's middle vertex sits on the axis and says nothing about
            // the ring's radius.
            if (r <= 1e-4f * width) continue;
            ringRadius = std::max(ringRadius, r);
            tightest = std::min(tightest, r);
            angles.push_back(std::atan2(flat.y, flat.x));
        }

        // Six sides is the fewest that still rounds: a square's corners stand
        // 41% proud of its own flats, so circumscribing one would put the
        // collider a long way outside the mesh. This is also what keeps a crate
        // a box.
        if (angles.size() < 6) return false;
        // One radius per ring, or it is not a ring.
        if (ringRadius - tightest > ringRadius * 0.01f) return false;

        // Evenly spread, so the circumscribing circle really does track the
        // silhouette rather than riding on one distant vertex.
        std::sort(angles.begin(), angles.end());
        float widestGap = angles.front() + 2.0f * glm::pi<float>() - angles.back();
        for (size_t i = 1; i < angles.size(); ++i) {
            widestGap = std::max(widestGap, angles[i] - angles[i - 1]);
        }
        if (widestGap > glm::radians(61.0f)) return false;

        fit.roundness = std::min(fit.roundness, std::cos(widestGap * 0.5f));
        fit.sides = fit.sides == 0 ? static_cast<int>(angles.size())
                                   : std::min(fit.sides, static_cast<int>(angles.size()));
        radii.push_back(ringRadius);
    }

    // The radius has to run STRAIGHT from one cap to the other. A model that
    // bulges or waists between them - the reactor does both - is not a frustum,
    // and fitting one to it would put its collider inside its own mesh.
    const float low = radii.front();
    const float high = radii.back();
    const float span = levels.back() - levels.front();
    if (span <= 1e-4f) return false;

    fit.profileError = 0.0f;
    for (size_t i = 0; i < levels.size(); ++i) {
        const float t = (levels[i] - levels.front()) / span;
        fit.profileError = std::max(fit.profileError, std::abs(radii[i] - (low + (high - low) * t)));
    }
    if (fit.profileError > std::max(low, high) * 0.02f) return false;

    fit.lowRadius = low;
    fit.highRadius = high;
    fit.halfHeight = span * 0.5f;
    fit.centre = glm::vec3(axis.x, 0.5f * (levels.front() + levels.back()), axis.y);
    fit.rings = static_cast<int>(levels.size());
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
