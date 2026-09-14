#version 460 core

// Additive, with a soft radial falloff so a bolt reads as a glow rather than a
// square. No texture: the shape is cheap to compute and one less asset to load.

in vec3 vColor;
in vec2 vUV;

out vec4 oColor;

void main() {
    float d = length(vUV * 2.0 - 1.0);
    float falloff = clamp(1.0 - d, 0.0, 1.0);
    falloff *= falloff;
    if (falloff <= 0.0) discard;
    oColor = vec4(vColor * falloff, 1.0);
}
