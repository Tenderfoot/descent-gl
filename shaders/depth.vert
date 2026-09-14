#version 460 core

// Depth prepass. Writes only depth, for every visible surface, so that each
// additive light pass can test GL_EQUAL and shade exactly the fragments that
// survive - no overdraw multiplied by the light count.

layout(location = 0) in vec3 aPos;

layout(std140, binding = 0) uniform Camera {
    mat4 uView;
    mat4 uProj;
};

uniform mat4 uModel;

// The gl_Position expression below must stay character-for-character identical
// to the one in depth.vert, and both are declared invariant.
//
// The lighting passes test GL_EQUAL against the depth the prepass wrote. Two
// shaders that compute the same position by a different association - say
// uProj * uView * (uModel * p) instead of uProj * uView * uModel * p - round
// differently in the last bit, the comparison fails for scattered fragments,
// and the wall comes out shredded with thin unlit lines. Nothing about that
// failure points at the vertex shader.
invariant gl_Position;

void main() {
    gl_Position = uProj * uView * uModel * vec4(aPos, 1.0);
}
