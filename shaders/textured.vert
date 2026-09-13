#version 460 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

layout(std140, binding = 0) uniform Camera {
    mat4 uView;
    mat4 uProj;
};

uniform mat4 uModel;

out vec2 vUV;
out vec3 vNormal;

void main() {
    vUV = aUV;
    vNormal = mat3(uModel) * aNormal;
    gl_Position = uProj * uView * uModel * vec4(aPos, 1.0);
}
