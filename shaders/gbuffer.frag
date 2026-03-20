#version 450

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
#include "input_structures.glsl"

layout (location = 0) in vec3 inWorldPos;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec2 inUV;
layout (location = 3) in vec4 inColor;
layout (location = 4) flat in uint inMaterialID;

// G-Buffer MRT outputs
layout (location = 0) out vec4 outAlbedo;       // RT0: rgb = albedo, a = unused
layout (location = 1) out vec4 outNormal;        // RT1: rgb = world normal ([-1,1] -> [0,1]), a = metallic
layout (location = 2) out vec4 outMaterial;      // RT2: r = roughness, g = ao, b = emissive, a = unused

void main()
{
    MaterialData mat = materials[inMaterialID];

    // --- Albedo ---
    vec4 baseColor = texture(globalTextures[nonuniformEXT(mat.colorTexID)], inUV) * mat.colorFactors ;
    outAlbedo = vec4(baseColor.rgb, 1.0);

    // --- Normal ---
    // Use interpolated vertex normal (no tangent-space normal mapping yet)
    vec3 N = normalize(inNormal);

    // Encode normal: [-1,1] -> [0,1]
    outNormal = vec4(N * 0.5 + 0.5, mat.metal_rough_factors.x);

    // --- Material ---
    vec4 metalRoughSample = texture(globalTextures[nonuniformEXT(mat.metalRoughTexID)], inUV);
    // glTF: metalRoughTex.b = metallic, metalRoughTex.g = roughness
    float roughness = metalRoughSample.g * mat.metal_rough_factors.y;
    float ao = 1.0; // placeholder, use occlusionTex later

    outMaterial = vec4(roughness, ao, 0.0, 0.0);
}
