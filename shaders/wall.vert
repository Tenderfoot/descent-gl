#version 460 core

// One light's additive contribution to the mine's walls.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in float aLayer;

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

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
flat out int vLayer;

void main() {
    vWorldPos = (uModel * vec4(aPos, 1.0)).xyz;
    vNormal = mat3(uModel) * aNormal;
    vUV = aUV;
    vLayer = int(aLayer + 0.5);
    gl_Position = uProj * uView * uModel * vec4(aPos, 1.0);
}
