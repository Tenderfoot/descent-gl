#version 460 core

in vec2 vUV;
in vec3 vNormal;

layout(binding = 0) uniform sampler2D uAlbedo;

out vec4 oColor;

void main() {
    vec3 albedo = texture(uAlbedo, vUV).rgb;

    // M0 has no light objects. This fixed directional term exists only so the
    // cube's faces are distinguishable while the skeleton is being verified.
    // M2 deletes it: from there on, surfaces start black and each light adds
    // its own contribution in a separate additive pass.
    vec3 n = normalize(vNormal);
    float ndl = max(dot(n, normalize(vec3(0.4, 0.85, 0.35))), 0.0);
    oColor = vec4(albedo * (0.30 + 0.70 * ndl), 1.0);
}
