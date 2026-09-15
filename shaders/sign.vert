#version 460 core

// World-space signage: the strips on the door slabs. Same vertex layout and
// same fragment shader as the HUD, because a sign is a HUD panel that happens
// to be bolted to a wall; only the transform differs, and it is the ordinary
// camera one.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

layout(std140, binding = 0) uniform Camera {
    mat4 uView;
    mat4 uProj;
};

out vec2 vUV;
out vec4 vColor;

void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = uProj * uView * vec4(aPos, 1.0);
}
