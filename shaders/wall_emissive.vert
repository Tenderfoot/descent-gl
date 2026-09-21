#version 460 core

// The emitting sides of the mine's walls, drawn once for the whole frame.
//
// Not a light pass: there is no light here, no normal and no shadow. A side
// with `emissive` set adds its own albedo to whatever the lights already put
// there, which is what makes a lava floor bright in a room with nothing else
// in it while leaving the rest of the mine black.

layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUV;
layout(location = 3) in float aLayer;
layout(location = 4) in float aEmissive;

layout(std140, binding = 0) uniform Camera {
    mat4 uView;
    mat4 uProj;
};

uniform mat4 uModel;

// The gl_Position expression below must stay character-for-character identical
// to the one in depth.vert, and both are declared invariant.
//
// This pass tests GL_EQUAL against the depth the prepass wrote, exactly as the
// light passes do. Two shaders that compute the same position by a different
// association - say uProj * uView * (uModel * p) instead of
// uProj * uView * uModel * p - round differently in the last bit, the
// comparison fails for scattered fragments, and the wall comes out shredded
// with thin unlit lines. Nothing about that failure points at the vertex
// shader.
invariant gl_Position;

out vec2 vUV;
flat out int vLayer;
flat out float vEmissive;

void main() {
    vUV = aUV;
    vLayer = int(aLayer + 0.5);
    vEmissive = aEmissive;
    gl_Position = uProj * uView * uModel * vec4(aPos, 1.0);
}
