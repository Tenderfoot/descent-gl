#version 460 core

in vec2 vUV;
in vec4 vColor;

layout(binding = 1) uniform sampler2D uFont;

out vec4 oColor;

void main() {
    // A negative UV is the sentinel for "solid quad, no texture", so bars and
    // glyphs come out of one draw call.
    if (vUV.x < 0.0) {
        oColor = vColor;
        return;
    }
    float mask = texture(uFont, vUV).a;
    if (mask <= 0.02) discard;
    oColor = vec4(vColor.rgb, vColor.a * mask);
}
