#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

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

// cubeModel() stretched to `size` - the whole edge along each axis, centred on
// the origin - with its UVs laid for a level block. The same 12 triangles in
// the same order, so it is as closed as the cube and casts the same way.
//
// `unitsPerTile` > 0 repeats the tile once per that many units along each
// face's edges, starting from the face's first corner; 0 or less stretches one
// tile across every face. tools/editor/src/geometry.js builds the same UVs for
// the editor's preview and has to be kept in step with this.
ModelData blockModel(const glm::vec3& size, float unitsPerTile);

// The model's axis-aligned bounds in its own space. Returns false for a model
// with no positions, leaving the outputs untouched.
bool modelBounds(const ModelData& model, glm::vec3& lo, glm::vec3& hi);

// A convex polyhedron, derived from a model's own faces.
//
// No hull algorithm is involved. For a model that is already convex its faces
// ARE its hull's faces, so the work is merging the triangles that share a plane
// - `pillar.obj`'s 28 become 10 - and recovering each face's boundary. A model
// that is NOT convex is rejected rather than approximated, because computing a
// real hull for it would produce a solid that is not the shape the renderer
// draws, and deciding how much of that to accept is a separate question.
struct ConvexHullFit {
    // Outward unit normal in xyz, plane offset in w: `dot(n, x) + d <= 0`
    // inside. The same sign convention the segment sides use, pointing the
    // other way, because a segment is a space you are inside and this is a
    // solid you are outside.
    std::vector<glm::vec4> planes;
    std::vector<glm::vec3> vertices;
    // Each plane's boundary polygon, wound counter-clockwise about its outward
    // normal: `face[i]` is `faceVertex[faceFirst[i] .. faceFirst[i + 1])`,
    // indexing `vertices`.
    std::vector<int> faceFirst;
    std::vector<int> faceVertex;

    float radius = 0.0f;        // the furthest vertex from `centre`
    glm::vec3 centre{0.0f};     // of the bounds, matching the box proxy
    glm::vec3 lo{0.0f};
    glm::vec3 hi{0.0f};
    // How far the worst vertex sits outside a face plane. Zero for a convex
    // model; reported so a model that just misses says by how much.
    float worstOutside = 0.0f;
};

// Beyond these a model is left on its bounding box. Nothing shipped comes near
// - the largest convex model is 16 vertices over 10 planes - and the point is
// that the sim step's cost per object stays something you can bound by reading
// the number, not by trusting the art.
inline constexpr int kMaxHullPlanes = 64;
inline constexpr int kMaxHullVertices = 128;

// True when the model is convex, in which case `fit` describes it exactly.
// False leaves the caller on `modelBounds`, and `fit.worstOutside` says how
// badly it missed.
bool modelConvexHull(const ModelData& model, ConvexHullFit& fit);

} // namespace dvk::gl
