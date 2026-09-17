#version 460 core

in vec2 vUV;
in vec4 vColor;

layout(binding = 1) uniform sampler2D uFont;
layout(binding = 3) uniform sampler2DArray uHudImages;
layout(binding = 4) uniform sampler2DArray uHudLogo;

out vec4 oColor;

void main() {
    // A negative UV is the sentinel for "solid quad, no texture", so bars and
    // glyphs come out of one draw call.
    if (vUV.x < 0.0) {
        oColor = vColor;
        return;
    }
    // Checked before the image array's band, which starts at 2.
    if (vUV.x > 3.5) {
        oColor = texture(uHudLogo, vec3(vUV.x - 4.0, vUV.y, 0.0)) * vColor;
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
