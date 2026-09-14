#version 460 core

// A full-screen triangle from gl_VertexID alone, for the stencil visualiser.
// No buffer, no VAO contents; bind an empty VAO and draw 3 vertices.
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    // Depth 1.0 is the NEAR plane under reversed-Z, so this passes a GEQUAL
    // test against anything and can be drawn with the depth test left on.
    gl_Position = vec4(p * 2.0 - 1.0, 1.0, 1.0);
}
