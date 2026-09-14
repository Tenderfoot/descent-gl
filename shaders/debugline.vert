#version 460 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;

layout(std140, binding = 0) uniform Camera {
    mat4 uView;
    mat4 uProj;
};

out vec3 vColor;

void main() {
    vColor = aColor;
    gl_Position = uProj * uView * vec4(aPos, 1.0);
}
