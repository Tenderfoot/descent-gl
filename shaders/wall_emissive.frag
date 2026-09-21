#version 460 core

// A side's own light, added on top of the light passes.
//
// There is still no ambient term in this project. Ambient would be a floor
// under every surface in the mine; this is one wall that emits, and everything
// around it stays exactly as black as it was. A glowing pool that should also
// lay light on the room needs a Light in the segment as well - the renderer has
// no area lights, and a shadow volume cast by a surface that is itself the
// source has no meaning.

in vec2 vUV;
flat in int vLayer;
flat in float vEmissive;

layout(binding = 0) uniform sampler2DArray uWalls;

// Seconds since the level started, from the fixed-step simulation rather than
// from a wall clock, so --frames/--screenshot still compares against an earlier
// run of the same length.
uniform float uTime;

// Per tile: xy scrolls the lookup in UV per second, z is the amplitude of the
// turbulence below. Indexed by layer, so one draw can carry sides using
// different tiles. Length must match kMaxWallTiles in render/wall_tiles.h.
uniform vec4 uTileAnim[32];

const float kTau = 6.28318530718;

// The old flowing-lava trick: warp the lookup by a sine of the other axis.
//
// The sines are in units of whole tiles - kTau * uv has period exactly 1 - so
// the warp at the end of a tile is the warp at the start of the next one, and
// the animation cannot pull a seam into a texture that tiles. The two axes run
// at slightly different rates so the pattern does not visibly cycle.
vec2 animate(vec2 uv, vec4 anim) {
    uv += anim.xy * uTime;
    if (anim.z > 0.0) {
        uv.x += sin(uv.y * kTau + uTime * 1.3) * anim.z;
        uv.y += sin(uv.x * kTau + uTime * 1.1) * anim.z;
    }
    return uv;
}

out vec4 oColor;

void main() {
    vec2 uv = animate(vUV, uTileAnim[vLayer]);
    vec3 albedo = texture(uWalls, vec3(uv, float(vLayer))).rgb;
    oColor = vec4(albedo * vEmissive, 1.0);
}
