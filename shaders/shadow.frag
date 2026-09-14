#version 460 core

// The stencil pass writes no colour at all. This only produces output for the
// debug wireframe mode.
uniform vec3 uColor;

out vec4 oColor;

void main() {
    oColor = vec4(uColor, 1.0);
}
