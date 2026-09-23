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
// Multiplies the albedo. White for everything healthy. There is no ambient
// term, so this only shows where light already falls - a disabled robot in an
// unlit corridor is marked by its emissive eyes instead, not by this.
uniform vec3 uAlbedoTint;

// Grates: cut-out panels in portals, which shadow by their texture's alpha.
// Every grate in the level is in this buffer, uploaded once per level; each
// light pass names the few its segment set spans. See Level::Grate.
struct Grate {
    vec4 plane;      // xyz normal, w = d
    vec4 uAxis;      // uv = (dot(uAxis.xyz, p) + uAxis.w, ...)
    vec4 vAxis;
    vec4 edges[4];   // inward edge planes; on the grate when all are >= 0
    ivec4 layer;     // x = texture layer
};
layout(std430, binding = 0) readonly buffer Grates {
    Grate uGrateData[];
};
const int kMaxLightGrates = 8;   // render/renderer.h
uniform int uGrateCount;
uniform int uGrates[kMaxLightGrates];

out vec4 oColor;

// How much of this light gets through every grate between it and p: the
// product of the alpha at each place the line from p to the light crosses one.
//
// Shaped so that every texture() call is made on every invocation. The loop
// count is a uniform, so the implicit derivatives - which pick the mip level,
// and are what keeps a bar pattern thrown far across a room from shimmering -
// are well defined; the tests only choose which samples count.
float grateTransmission(vec3 p) {
    // A fragment within this of a grate's plane is on the grate - its own bars
    // are lit, not shadowed by themselves - and a light that close is on it.
    const float kPlaneMargin = 0.01;
    // Leans towards shadow at the rim, where the grate meets the rock and a
    // line that crosses exactly there would otherwise slip past both.
    const float kEdgeMargin = 0.01;

    float transmit = 1.0;
    for (int i = 0; i < uGrateCount; ++i) {
        Grate g = uGrateData[uGrates[i]];
        float sp = dot(g.plane.xyz, p) + g.plane.w;
        float sl = dot(g.plane.xyz, uLightPos) + g.plane.w;
        float denom = sp - sl;
        float t = abs(denom) > 1e-6 ? sp / denom : 0.0;
        vec3 x = mix(p, uLightPos, t);

        vec2 uv = vec2(dot(g.uAxis.xyz, x) + g.uAxis.w, dot(g.vAxis.xyz, x) + g.vAxis.w);
        float alpha = texture(uWalls, vec3(uv, float(g.layer.x))).a;

        bool crosses = sp * sl < 0.0 && abs(sp) > kPlaneMargin && abs(sl) > kPlaneMargin;
        bool inside = true;
        for (int e = 0; e < 4; ++e) {
            inside = inside && dot(g.edges[e].xyz, x) + g.edges[e].w > -kEdgeMargin;
        }
        if (crosses && inside) transmit *= alpha;
    }
    return transmit;
}

void main() {
    int layer = uLayerOverride >= 0 ? uLayerOverride : vLayer;
    vec3 albedo = texture(uWalls, vec3(vUV, float(layer))).rgb * uAlbedoTint;
    // Before any discard below, for the derivatives' sake: see the function.
    float transmit = grateTransmission(vWorldPos);

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

    attenuation *= transmit;

    float ndl = max(dot(n, l), 0.0);
    if (ndl <= 0.0 || attenuation <= 0.0) discard;

    vec3 v = normalize(uEyePos - vWorldPos);
    vec3 h = normalize(l + v);
    float spec = pow(max(dot(n, h), 0.0), 48.0) * 0.25;

    oColor = vec4((albedo * ndl + vec3(spec)) * uLightColor * attenuation, 1.0);
}
