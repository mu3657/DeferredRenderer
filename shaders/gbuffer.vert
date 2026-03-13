#version 450

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

#include "input_structures.glsl"

layout (location = 0) out vec3 outWorldPos;
layout (location = 1) out vec3 outNormal;
layout (location = 2) out vec2 outUV;
layout (location = 3) out vec4 outColor;

struct Vertex {
    vec3 position;
    float uv_x;
    vec3 normal;
    float uv_y;
    vec4 color;
};

layout(buffer_reference, std430) readonly buffer VertexBuffer{
    Vertex vertices[];
};

//push constants block
layout( push_constant ) uniform constants
{
    mat4 render_matrix;
    VertexBuffer vertexBuffer;
} PushConstants;

void main()
{
    Vertex v = PushConstants.vertexBuffer.vertices[gl_VertexIndex];

    vec4 worldPos = PushConstants.render_matrix * vec4(v.position, 1.0);
    gl_Position = sceneData.viewproj * worldPos;

    outWorldPos = worldPos.xyz;
    outNormal = normalize(mat3(PushConstants.render_matrix) * v.normal);
    outUV = vec2(v.uv_x, v.uv_y);
    outColor = v.color;
}
