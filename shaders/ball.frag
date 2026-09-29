#version 460 core

// Emissive, and nothing lights it: the ball is the light. One flat colour would
// read as a disc, though, so it is brightest where it faces the eye and falls
// off towards the rim - a glowing ball rather than a coloured coin. The centre
// is pushed towards white because the target is 8-bit: the walls beside the
// ball are lit by it at more than 1.0 and clip, and a ball drawn at exactly its
// own colour would look dimmer than the wall it is lighting.

in vec3 vWorld;
in vec3 vNormal;

uniform vec3 uColor;
uniform vec3 uEye;

out vec4 oColor;

void main() {
    vec3 n = normalize(vNormal);
    vec3 v = normalize(uEye - vWorld);
    float facing = clamp(dot(n, v), 0.0, 1.0);

    vec3 body = uColor * mix(0.5, 1.0, facing);
    float core = pow(facing, 6.0);
    oColor = vec4(mix(body, vec3(1.0), core * 0.45), 1.0);
}
