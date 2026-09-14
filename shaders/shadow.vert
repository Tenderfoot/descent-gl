#version 460 core

// Shadow volume geometry, already in world space.
//
// w = 1 is an ordinary vertex. w = 0 is a vertex extruded to infinity: its xyz
// carries the DIRECTION away from the light rather than a position. Under the
// project's infinite far plane (see render/camera.h) such a vertex lands
// exactly on the far plane instead of being clipped, which is the whole reason
// that projection exists.

layout(location = 0) in vec4 aPos;

layout(std140, binding = 0) uniform Camera {
    mat4 uView;
    mat4 uProj;
};

// The occluder's own model matrix - identity for the mine, the mesh's matrix
// for the ship, a robot, a pillar or a door slab - and the expression below is
// character-for-character the one in depth.vert.
//
// The volume's front cap is the occluder's own light-facing triangles, exactly
// coincident with the geometry the depth prepass drew. Depth-fail only counts a
// face that is STRICTLY behind the surface, so a coincident cap must compare
// exactly equal and change nothing. Compute it by a different association -
// including transforming to world space on the CPU and drawing with identity
// here - and the last bit differs, scattered pixels count as behind, and the
// surface fizzes with false self-shadow.
uniform mat4 uModel;

invariant gl_Position;

void main() {
    gl_Position = uProj * uView * uModel * aPos;
}
