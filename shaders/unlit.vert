#version 460 core

// The unlit pass: emissive surfaces and debug geometry, drawn once at full
// brightness after every light has had its turn. From M5 this carries bolts,
// flares and explosions; from M7 the HUD.

layout(location = 0) in vec3 aPos;

layout(std140, binding = 0) uniform Camera {
    mat4 uView;
    mat4 uProj;
};

uniform mat4 uModel;

invariant gl_Position;

void main() {
    gl_Position = uProj * uView * uModel * vec4(aPos, 1.0);
}
