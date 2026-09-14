#version 460 core

// Additive per-light shading. There is no ambient term anywhere in this
// project: a surface no light reaches stays at the clear colour, which is
// black. The darkness is the absence of a pass, not a grade, and it is what
// makes the headlight and flares of later milestones into mechanics.

in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
flat in int vLayer;

layout(binding = 0) uniform sampler2DArray uWalls;

uniform vec3 uLightPos;
uniform vec3 uLightColor;     // colour * intensity
uniform float uLightRadius;
uniform vec3 uLightDir;       // spot only, normalised, pointing away from the light
uniform float uCosInner;
uniform float uCosOuter;
uniform int uIsSpot;
uniform vec3 uEyePos;
// >= 0 overrides the per-vertex layer, for meshes that carry no layer
// attribute. Lets the ship and the walls share one lit pass.
uniform int uLayerOverride;

out vec4 oColor;

void main() {
    int layer = uLayerOverride >= 0 ? uLayerOverride : vLayer;
    vec3 albedo = texture(uWalls, vec3(vUV, float(layer))).rgb;

    vec3 n = normalize(vNormal);
    vec3 toLight = uLightPos - vWorldPos;
    float dist = length(toLight);
    if (dist > uLightRadius) discard;
    vec3 l = toLight / max(dist, 1e-4);

    // Windowed inverse square, in units of the light's own radius.
    //
    // The window is what makes the light reach exactly zero AT the radius
    // instead of at infinity, which is what lets the segment set be an exact
    // bound rather than an approximation. Hitting zero matters: a light still
    // faintly lit at the edge of its set would pop as the set cut it off.
    //
    // The inverse-square term is in t = dist/radius rather than in world
    // units. Absolute inverse square is only meaningful if intensity is in
    // absolute units too, and at this project's scale - 20-unit segments - that
    // means authoring lights in the hundreds and getting a pinprick of light at
    // the source with blackness two metres away. Radius-relative falloff gives
    // one knob that means what it looks like it means: radius sets both the
    // reach and the shape, and intensity stays a small readable number.
    float t = dist / uLightRadius;
    float window = clamp(1.0 - t * t * t * t, 0.0, 1.0);
    float attenuation = window * window / (1.0 + 12.0 * t * t);

    if (uIsSpot != 0) {
        float cosAngle = dot(-l, uLightDir);
        attenuation *= smoothstep(uCosOuter, uCosInner, cosAngle);
    }

    float ndl = max(dot(n, l), 0.0);
    if (ndl <= 0.0 || attenuation <= 0.0) discard;

    vec3 v = normalize(uEyePos - vWorldPos);
    vec3 h = normalize(l + v);
    float spec = pow(max(dot(n, h), 0.0), 48.0) * 0.25;

    oColor = vec4((albedo * ndl + vec3(spec)) * uLightColor * attenuation, 1.0);
}
