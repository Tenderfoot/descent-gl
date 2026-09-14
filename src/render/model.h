#pragma once

#include "render/mesh.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <string>
#include <vector>

namespace dvk::gl {

// An OBJ as authored, before it is expanded for the GPU.
//
// Two representations are kept because two systems need different things.
// Rendering wants flat per-face normals, which means one vertex per triangle
// corner and no sharing. M3's shadow volumes want the opposite: the welded
// topology, where a shared edge really is one edge with two triangles on it, so
// silhouettes and closure can be computed. Expanding first and trying to weld
// back afterwards loses exactly the information that matters, so the welded
// form is what the loader produces and toMesh() expands from it.
struct ModelData {
    std::vector<glm::vec3> positions;   // welded, in authoring order
    std::vector<glm::uvec3> triangles;  // indices into positions
    std::vector<glm::vec2> uvs;         // one per triangle corner, 3 per triangle

    size_t triangleCount() const { return triangles.size(); }
};

// Loads an OBJ with tinyobjloader. Positions are welded by index as authored,
// so a file whose faces reference the same vertex index really do share it.
// A missing or unparsable file is fatal.
ModelData loadObj(const std::string& path);

// Expands to a drawable mesh with flat per-face normals.
Mesh toMesh(const ModelData& model);

} // namespace dvk::gl
