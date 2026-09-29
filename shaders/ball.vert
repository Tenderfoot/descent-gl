#version 460 core

// A floating light's ball, in the unlit pass. The mesh is a unit sphere whose
// normals point straight out, and uModel is only ever a translate and a
// uniform scale, so the normal needs no inverse-transpose.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;

layout(std140, binding = 0) uniform Camera {
    mat4 uView;
    mat4 uProj;
};

uniform mat4 uModel;

out vec3 vWorld;
out vec3 vNormal;

void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    vWorld = world.xyz;
    vNormal = aNormal;
    gl_Position = uProj * uView * world;
}
