#version 460 core

in vec3 vColor;
out vec4 oColor;

void main() {
    oColor = vec4(vColor, 1.0);
}
