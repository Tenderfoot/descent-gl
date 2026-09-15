#pragma once

#include "render/mesh.h"
#include "render/model_data.h"

namespace dvk::gl {

// `ModelData`, `loadObj`, `cubeModel` and `modelBounds` live in
// `render/model_data.h`, which has no GL in it and can therefore be included
// from outside the renderer. What is left here is the part that needs a GPU.

// Expands to a drawable mesh with flat per-face normals. Corners come out in
// the triangle's own `x, y, z` order, so the drawn triangle is the source
// triangle - which is what lets an occluder built from the same ModelData cap
// its own surface exactly. See cubeModel() in model_data.h.
Mesh toMesh(const ModelData& model);

} // namespace dvk::gl
