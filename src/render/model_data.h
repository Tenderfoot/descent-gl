#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <string>
#include <vector>

// An OBJ as authored, with no GL in sight.
//
// Split out of `render/model.h` so that things which need a model's GEOMETRY
// but not its drawable form can have it. `model.h` pulls in `render/mesh.h` and
// therefore `render/gl.h`, which is off limits outside `src/render`; this
// header is glm and the standard library, and the game layer includes it to
// derive collision proxies from the same file the renderer draws.
//
// `sim` still does not include this, and must not. It is handed finished
// `ProxyVolume` values instead - see `src/game/proxies.h`.

namespace dvk::gl {

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

// A unit cube centred on the origin: 8 welded positions, 12 triangles, closed.
//
// In ModelData form rather than as a finished Mesh, because anything that both
// DRAWS a cube and CASTS a shadow from one has to derive the drawable mesh and
// the OccluderMesh from the same triangle list. The shadow volume's front cap
// is the occluder's own light-facing triangles sitting exactly on the surface
// the depth prepass drew, and "exactly" is a bit-for-bit claim about the
// rasterized interior, not only about the vertices: two triangulations of the
// same flat quad across opposite diagonals describe the same plane, but the
// hardware sets that plane up from different corners and the interior lands an
// ulp apart in a scattered pattern. See render/shadow.h.
//
// The door slabs had precisely that bug. They were drawn from one hand-written
// cube and cast from another, the two agreed on four faces and split the other
// two across the opposite diagonal, and the door fizzed with false self-shadow
// on one face and was clean on the other.
ModelData cubeModel();

// The model's axis-aligned bounds in its own space. Returns false for a model
// with no positions, leaving the outputs untouched.
bool modelBounds(const ModelData& model, glm::vec3& lo, glm::vec3& hi);

} // namespace dvk::gl
