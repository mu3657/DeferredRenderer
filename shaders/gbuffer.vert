#version 450

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

#include "input_structures.glsl"

layout (location = 0) out vec3 outWorldPos;
layout (location = 1) out vec3 outNormal;
layout (location = 2) out vec2 outUV;
layout (location = 3) out vec4 outColor;
layout (location = 4) flat out uint outMaterialID;
layout (location = 5) out vec4 outTangent;
layout (location = 6) out vec4 outPreviousClip;
layout (location = 7) flat out uint outHistoryValid;

struct Vertex {
    vec3 position;
    float uv_x;
    vec3 normal;
    float uv_y;
    vec4 color;
    vec4 tangent;
};

layout(buffer_reference, std430) readonly buffer VertexBuffer{
    Vertex vertices[];
};

layout(buffer_reference, std430) readonly buffer PreviousTransform {
    mat4 worldMatrix;
    uvec4 valid;
};

//push constants block
layout( push_constant ) uniform constants
{
    mat4 render_matrix;
    VertexBuffer vertexBuffer;
    uint materialID;
    uint hasPreviousTransform;
    layout(offset = 80) PreviousTransform previousTransform;
} PushConstants;

void main()
{
    Vertex v = PushConstants.vertexBuffer.vertices[gl_VertexIndex];

    vec4 worldPos = PushConstants.render_matrix * vec4(v.position, 1.0);
    gl_Position = sceneData.viewproj * worldPos;
    outPreviousClip = gl_Position;
    outHistoryValid = 0u;
    // The same vertex shader is used by forward transparency. Its draw has no
    // temporal record, so never dereference a null buffer-device address.
    if (PushConstants.hasPreviousTransform != 0u) {
        outPreviousClip = sceneData.previousViewproj
            * PushConstants.previousTransform.worldMatrix * vec4(v.position, 1.0);
        outHistoryValid = PushConstants.previousTransform.valid.x;
    }

    outWorldPos = worldPos.xyz;
    mat3 normalMatrix = transpose(inverse(mat3(PushConstants.render_matrix)));
    outNormal = normalize(normalMatrix * v.normal);
    mat3 objectToWorld = mat3(PushConstants.render_matrix);
    vec3 worldTangent = objectToWorld * v.tangent.xyz;
    worldTangent -= outNormal * dot(outNormal, worldTangent);
    float tangentLengthSquared = dot(worldTangent, worldTangent);
    if (tangentLengthSquared <= 1e-8) {
        vec3 fallbackAxis = abs(outNormal.z) < 0.999
            ? vec3(0.0, 0.0, 1.0)
            : vec3(0.0, 1.0, 0.0);
        worldTangent = normalize(cross(fallbackAxis, outNormal));
    } else {
        worldTangent *= inversesqrt(tangentLengthSquared);
    }
    float transformHandedness = determinant(objectToWorld) < 0.0 ? -1.0 : 1.0;
    outTangent = vec4(worldTangent, v.tangent.w * transformHandedness);
    outUV = vec2(v.uv_x, v.uv_y);
    outColor = v.color;
    outMaterialID = PushConstants.materialID;
}
