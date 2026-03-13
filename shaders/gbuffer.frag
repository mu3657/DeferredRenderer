#version 450

#extension GL_GOOGLE_include_directive : require
#include "input_structures.glsl"

layout (location = 0) in vec3 inWorldPos;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec2 inUV;
layout (location = 3) in vec4 inColor;

// G-Buffer MRT outputs
layout (location = 0) out vec4 outAlbedo;       // RT0: rgb = albedo, a = unused
layout (location = 1) out vec4 outNormal;        // RT1: rgb = world normal ([-1,1] -> [0,1]), a = metallic
layout (location = 2) out vec4 outMaterial;      // RT2: r = roughness, g = ao, b = emissive, a = unused

void main()
{
    // --- Albedo ---
    vec4 baseColor = texture(colorTex, inUV) * materialData.colorFactors ;
    outAlbedo = vec4(baseColor.rgb, 1.0);

    // --- Normal ---
    // Use interpolated vertex normal (no tangent-space normal mapping yet)
    vec3 N = normalize(inNormal);

    // If you add normalTex (binding 3) later, you can do TBN here via screen-space derivatives:
    // vec3 dPdx = dFdx(inWorldPos);
    // vec3 dPdy = dFdy(inWorldPos);
    // vec2 dUVdx = dFdx(inUV);
    // vec2 dUVdy = dFdy(inUV);
    // vec3 T = normalize(dPdx * dUVdy.y - dPdy * dUVdx.y);
    // vec3 B = normalize(cross(N, T));
    // mat3 TBN = mat3(T, B, N);
    // vec3 normalMap = texture(normalTex, inUV).rgb * 2.0 - 1.0;
    // N = normalize(TBN * normalMap);

    // Encode normal: [-1,1] -> [0,1]
    outNormal = vec4(N * 0.5 + 0.5, materialData.metal_rough_factors.x);

    // --- Material ---
    vec4 metalRoughSample = texture(metalRoughTex, inUV);
    // glTF: metalRoughTex.b = metallic, metalRoughTex.g = roughness
    float roughness = metalRoughSample.g * materialData.metal_rough_factors.y;
    float ao = 1.0; // placeholder, use occlusionTex later

    outMaterial = vec4(roughness, ao, 0.0, 0.0);
}
