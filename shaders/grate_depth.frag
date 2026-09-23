#version 460 core

// Depth prepass for grates, behind wall.vert. The same job as depth.frag, with
// the gaps in the grate cut out, so that the walls behind it keep the depth and
// the light passes shade them through the holes.
//
// The threshold is the only place the cut-out's edge is decided. The lit pass
// draws the grate again with GL_EQUAL and needs no test of its own: a sample
// discarded here holds the depth of whatever is behind it, and the grate's own
// fragment there cannot match it.

in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
flat in int vLayer;

layout(binding = 0) uniform sampler2DArray uWalls;

void main() {
    if (texture(uWalls, vec3(vUV, float(vLayer))).a < 0.5) discard;
}
