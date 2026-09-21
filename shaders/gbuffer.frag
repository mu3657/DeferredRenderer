#version 450

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
#include "input_structures.glsl"

layout (location = 0) in vec3 inWorldPos;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec2 inUV;
layout (location = 3) in vec4 inColor;
layout (location = 4) flat in uint inMaterialID;
layout (location = 5) in vec4 inTangent;

// G-Buffer MRT outputs
layout (location = 0) out vec4 outAlbedo;       // RT0: rgb = albedo, a = unused
layout (location = 1) out vec4 outNormal;        // RT1: rgb = world normal ([-1,1] -> [0,1]), a = metallic
layout (location = 2) out vec4 outMaterial;      // RT2: r = roughness, g = ao, b/a = reserved
layout (location = 3) out vec4 outEmissive;      // RT3: rgb = linear HDR emissive

void main()
{
    MaterialData mat = materials[inMaterialID];

    // --- Albedo ---
    vec4 baseColor = texture(globalTextures[nonuniformEXT(mat.colorTexID)], inUV)
        * mat.colorFactors;
    if ((mat.materialFlags & MATERIAL_FLAG_ALPHA_MASK) != 0u
        && baseColor.a < clamp(mat.emissive_factors.w, 0.0, 1.0)) {
        discard;
    }
    outAlbedo = vec4(baseColor.rgb, 1.0);

    // --- Normal ---
    vec3 N = normalize(inNormal);
    // Preserve the authored normal for legacy two-sided rasterization. Its
    // winding is not a reliable shading-side contract (including mirrored
    // instances), so gl_FrontFacing must not unconditionally invert it.
    // Legacy PNCV assets do not contain authored tangents. A generated basis is
    // useful for diagnostics, but it is not safe to enable normal mapping across
    // mirrored seams automatically without validating the tangent contract.
    if ((mat.materialFlags & MATERIAL_FLAG_TANGENT_SPACE_READY) != 0u) {
        vec3 T = inTangent.xyz - N * dot(N, inTangent.xyz);
        float tangentLengthSquared = dot(T, T);
        if (tangentLengthSquared <= 1e-8) {
            vec3 fallbackAxis = abs(N.z) < 0.999
                ? vec3(0.0, 0.0, 1.0)
                : vec3(0.0, 1.0, 0.0);
            T = normalize(cross(fallbackAxis, N));
        } else {
            T *= inversesqrt(tangentLengthSquared);
        }
        vec3 B = normalize(cross(N, T)) * inTangent.w;
        vec3 tangentNormal = texture(
            globalTextures[nonuniformEXT(mat.normalTexID)], inUV).xyz * 2.0 - 1.0;
        tangentNormal.xy *= mat.metal_rough_factors.z;
        N = normalize(mat3(T, B, N) * tangentNormal);
    }

    vec4 metalRoughSample = texture(globalTextures[nonuniformEXT(mat.metalRoughTexID)], inUV);
    float metallic = clamp(metalRoughSample.b * mat.metal_rough_factors.x, 0.0, 1.0);
    float roughness = clamp(metalRoughSample.g * mat.metal_rough_factors.y, 0.04, 1.0);
    // Filter high-frequency normal variation into perceptual roughness. Without
    // this, sub-pixel normal-map changes can collapse GGX into isolated hot pixels.
    vec3 normalDx = dFdx(N);
    vec3 normalDy = dFdy(N);
    float normalVariance = 0.5 * (
        dot(normalDx, normalDx) + dot(normalDy, normalDy));
    float kernelRoughnessSquared = min(2.0 * normalVariance, 0.18);
    roughness = sqrt(clamp(
        roughness * roughness + kernelRoughnessSquared,
        0.0016,
        1.0));
    float occlusionSample = clamp(
        texture(globalTextures[nonuniformEXT(mat.occlusionTexID)], inUV).r,
        0.0,
        1.0);
    float ao = mix(1.0, occlusionSample, clamp(mat.metal_rough_factors.w, 0.0, 1.0));
    vec3 emissive = texture(
        globalTextures[nonuniformEXT(mat.emissiveTexID)], inUV).rgb
        * mat.emissive_factors.rgb;

    // Encode normal: [-1,1] -> [0,1]
    outNormal = vec4(N * 0.5 + 0.5, metallic);

    // --- Material ---
    outMaterial = vec4(roughness, ao, 0.0, 0.0);
    outEmissive = vec4(max(emissive, vec3(0.0)), 1.0);
}
