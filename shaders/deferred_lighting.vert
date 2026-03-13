#version 450

// Fullscreen triangle vertex shader (no VBO needed)
layout (location = 0) out vec2 outUV;

void main()
{
    // Generate an oversized triangle covering the screen:
    // gl_VertexIndex:   0,  1,  2
    // outUV:         (0,0),(2,0),(0,2)
    outUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);

    // Map UVs to NDC coordinates [-1, 1]
    gl_Position = vec4(outUV * 2.0f - 1.0f, 0.0f, 1.0f);
}
