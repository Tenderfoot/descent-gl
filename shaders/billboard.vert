#version 460 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
layout(location = 2) in vec2 aUV;
layout(location = 3) in float aLayer;

layout(std140, binding = 0) uniform Camera {
    mat4 uView;
    mat4 uProj;
};

out vec3 vColor;
out vec2 vUV;
flat out int vLayer;

void main() {
    vColor = aColor;
    vUV = aUV;
    vLayer = int(aLayer);
    gl_Position = uProj * uView * vec4(aPos, 1.0);
}
