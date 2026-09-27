#version 460 core

// Additive. Most quads have no texture at all: a soft radial falloff is cheap
// to compute and reads as a glow, which is all a bolt or an impact flash is.
// A negative layer is the sentinel for that case, the same trick hud.frag uses
// with a negative UV, so hand-drawn pickup sprites and procedural glows come
// out of one draw call.
//
// The sprite array holds premultiplied colour, so the fade at a sprite's edge
// is already in the RGB - which is what an additive pass needs, since it never
// looks at alpha.

in vec3 vColor;
in vec2 vUV;
flat in int vLayer;

layout(binding = 2) uniform sampler2DArray uSprites;

out vec4 oColor;

void main() {
    // A shockwave ring (Billboards::kRing): a thin bright band peaking at
    // kRingPeak of the half size, which is the blast's edge, over a faint fill
    // that says the whole disc inside it was in the blast.
    if (vLayer == -2) {
        float d = length(vUV * 2.0 - 1.0);
        if (d > 1.0) discard;
        float band = clamp(1.0 - abs(d - 0.96) / 0.04, 0.0, 1.0);
        band *= band;
        float fill = d < 0.96 ? 0.08 * d * d : 0.0;
        if (band + fill <= 0.0) discard;
        oColor = vec4(vColor * (band + fill), 1.0);
        return;
    }
    if (vLayer < 0) {
        float d = length(vUV * 2.0 - 1.0);
        float falloff = clamp(1.0 - d, 0.0, 1.0);
        falloff *= falloff;
        if (falloff <= 0.0) discard;
        oColor = vec4(vColor * falloff, 1.0);
        return;
    }

    vec3 sprite = texture(uSprites, vec3(vUV, float(vLayer))).rgb;
    if (dot(sprite, vec3(1.0)) <= 0.0) discard;
    oColor = vec4(sprite * vColor, 1.0);
}
