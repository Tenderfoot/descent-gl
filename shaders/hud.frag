#version 460 core

in vec2 vUV;
in vec4 vColor;

layout(binding = 1) uniform sampler2D uFont;
layout(binding = 3) uniform sampler2DArray uHudImages;

out vec4 oColor;

void main() {
    // A negative UV is the sentinel for "solid quad, no texture", so bars and
    // glyphs come out of one draw call.
    if (vUV.x < 0.0) {
        oColor = vColor;
        return;
    }
    if (vUV.x > 1.5) {
        float layer = floor(vUV.y * 0.5);
        vec2 imageUV = vec2(vUV.x - 2.0, vUV.y - layer * 2.0);
        oColor = texture(uHudImages, vec3(imageUV, layer)) * vColor;
        return;
    }
    float mask = texture(uFont, vUV).a;
    if (mask <= 0.02) discard;
    oColor = vec4(vColor.rgb, vColor.a * mask);
}
