#version 450

#extension GL_GOOGLE_include_directive : require
#define LIGHTING_PASS 1
#define USE_LIGHT_DATA 1
#include "input_structures.glsl"

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outFragColor;

// New Descriptor Set for the Lighting Pass
layout(set = 1, binding = 0) uniform sampler2D gAlbedo;
layout(set = 1, binding = 1) uniform sampler2D gNormal;
layout(set = 1, binding = 2) uniform sampler2D gMaterial;
layout(set = 1, binding = 3) uniform sampler2D gDepth;
layout(set = 3, binding = 0) uniform sampler2D shadowMap;
layout(set = 4, binding = 0) uniform sampler2D contactShadowTexture;

const int SHADOW_CASCADE_COUNT = 4;

layout(set = 3, binding = 1) uniform ShadowDataBuffer {
    mat4 lightViewProj[SHADOW_CASCADE_COUNT];
    vec4 cascadeSplits;
    vec4 cascadeBlendWidths;
    vec4 pcfKernelRadii;
    vec4 cascadeTexelWorldSizes;
    vec4 cascadeDepthRanges;
    vec4 lightDir;
    vec4 params; // x = bias in shadow texels, y = strength, z = texelSize, w = enabled
} shadowData;

// Basic PBR Lighting functions (simplified for minimalism)
const float PI = 3.14159265359;

struct ShadowCascadeSelection {
    int primary;
    int secondary;
    float blend;
};

ShadowCascadeSelection selectShadowCascades(float viewDepth)
{
    if (viewDepth > shadowData.cascadeSplits.w) {
        return ShadowCascadeSelection(-1, -1, 0.0);
    }

    for (int cascadeIndex = 0; cascadeIndex < SHADOW_CASCADE_COUNT - 1; cascadeIndex++) {
        float split = shadowData.cascadeSplits[cascadeIndex];
        float blendWidth = shadowData.cascadeBlendWidths[cascadeIndex];
        if (blendWidth > 0.0 && viewDepth >= split - blendWidth && viewDepth <= split + blendWidth) {
            float blend = smoothstep(split - blendWidth, split + blendWidth, viewDepth);
            return ShadowCascadeSelection(cascadeIndex, cascadeIndex + 1, blend);
        }
        if (viewDepth < split - blendWidth) {
            return ShadowCascadeSelection(cascadeIndex, -1, 0.0);
        }
    }

    return ShadowCascadeSelection(SHADOW_CASCADE_COUNT - 1, -1, 0.0);
}

vec2 cascadeAtlasOffset(int cascadeIndex)
{
    return vec2(float(cascadeIndex % 2), float(cascadeIndex / 2)) * 0.5;
}

float sampleShadowCascade(int cascadeIndex, vec3 worldPos, vec3 N, vec3 L)
{
    vec4 lightClip = shadowData.lightViewProj[cascadeIndex] * vec4(worldPos, 1.0);
    vec3 lightNdc = lightClip.xyz / lightClip.w;

    vec2 localShadowUV = lightNdc.xy * 0.5 + 0.5;
    float receiverDepth = lightNdc.z;

    if (lightClip.w <= 0.0 ||
    any(lessThan(localShadowUV, vec2(0.0))) ||
    any(greaterThan(localShadowUV, vec2(1.0))) ||
    receiverDepth < 0.0 ||
    receiverDepth > 1.0) {
        return 1.0;
    }

    float ndotl = max(dot(N, L), 0.0);
    float texelWorldSize = shadowData.cascadeTexelWorldSizes[cascadeIndex];
    float depthRange = max(shadowData.cascadeDepthRanges[cascadeIndex], 0.0001);
    float biasWorld = texelWorldSize * shadowData.params.x * mix(2.0, 1.0, ndotl);
    float bias = biasWorld / depthRange;
    float texelSize = shadowData.params.z;
    int kernelRadius = clamp(int(shadowData.pcfKernelRadii[cascadeIndex] + 0.5), 0, 3);

    float visibility = 0.0;
    vec2 atlasOffset = cascadeAtlasOffset(cascadeIndex);
    for (int y = -kernelRadius; y <= kernelRadius; y++) {
        for (int x = -kernelRadius; x <= kernelRadius; x++) {
            vec2 offset = vec2(x, y) * texelSize;
            vec2 localSampleUV = clamp(
                localShadowUV + offset,
                vec2(texelSize * 0.5),
                vec2(1.0 - texelSize * 0.5));
            vec2 atlasUV = atlasOffset + localSampleUV * 0.5;
            float closestDepth = texture(shadowMap, atlasUV).r;

            visibility += (receiverDepth + bias < closestDepth) ? 0.0 : 1.0;
        }
    }

    float pcfSampleCount = float((kernelRadius * 2 + 1) * (kernelRadius * 2 + 1));
    float pcfVisibility = visibility / pcfSampleCount;
    float shadowStrength = clamp(shadowData.params.y, 0.0, 1.0);
    return mix(1.0, pcfVisibility, shadowStrength);
}

float sampleDirectionalShadow(vec3 worldPos, vec3 N, vec3 L)
{
    if (shadowData.params.w <= 0.0) {
        return 1.0;
    }

    float viewDepth = -(sceneData.view * vec4(worldPos, 1.0)).z;
    ShadowCascadeSelection selection = selectShadowCascades(viewDepth);
    if (selection.primary < 0) {
        return 1.0;
    }

    float visibility = sampleShadowCascade(selection.primary, worldPos, N, L);
    if (selection.secondary >= 0) {
        float nextVisibility = sampleShadowCascade(selection.secondary, worldPos, N, L);
        visibility = mix(visibility, nextVisibility, selection.blend);
    }

    return visibility;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness*roughness;
    float a2 = a*a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH*NdotH;

    float nom   = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return nom / denom;
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r*r) / 8.0;

    float nom   = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return nom / denom;
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

vec3 evaluatePBRDirect(vec3 albedo, float metallic, float roughness, vec3 N, vec3 V, vec3 L, vec3 radiance) {
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);

    if (NdotL <= 0.0) {
        return vec3(0.0);
    }

    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);

    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * NdotL + 0.0001;
    vec3 specular = numerator / denominator;

    vec3 kS = F;
    vec3 kD = vec3(1.0) - kS;
    kD *= 1.0 - metallic;

    return (kD * albedo / PI + specular) * radiance * NdotL;
}

#include "area_light.glsl"

// Reconstruct world position from depth
vec3 reconstructPosition(vec2 uv, float depth, mat4 invViewProj) {
    vec4 clipSpace = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 worldSpace = invViewProj * clipSpace;
    return worldSpace.xyz / worldSpace.w;
}

void main()
{
    // 1. Sample G-Buffer
    vec4 albedoSample   = texture(gAlbedo, inUV);
    vec4 normalSample   = texture(gNormal, inUV); // Normal encoded as 0..1, a=metallic
    vec4 materialSample = texture(gMaterial, inUV); // r=roughness, g=ao
    float depth         = texture(gDepth, inUV).r;

    // Reverse-Z depth clears the background to 0. Discard keeps the pre-drawn background.
    if (depth <= 0.0) {
        discard;
    }

    // 2. Unpack attributes
    vec3 albedo     = albedoSample.rgb;
    vec3 N          = normalize(normalSample.xyz * 2.0 - 1.0);
    float metallic  = normalSample.a;
    float roughness = materialSample.r;
    float ao        = materialSample.g;
    
    // We need inverse view-projection to reconstruct the world pos
    mat4 invViewProj = inverse(sceneData.viewproj);
    vec3 worldPos = reconstructPosition(inUV, depth, invViewProj);
    
    // Camera position (can be extracted from inverse view matrix)
    vec3 camPos = inverse(sceneData.view)[3].xyz;
    vec3 V = normalize(camPos - worldPos);

    vec3 Lo = vec3(0.0);

    uint directionalSeen = 0;
    // --- Direct Lighting ---
    for (uint i = 0; i < lightData.lightCount; i++) {
        float visibility = 1.0;

        GPULight light = lights[i];
        uint type = uint(light.directionType.w + 0.5);

        if (type == LIGHT_TYPE_RECT_AREA) {
            Lo += evaluateRectAreaLight(albedo, metallic, roughness, N, V, worldPos, light);
            continue;
        }

        vec3 L = vec3(0.0);
        vec3 radiance = light.colorIntensity.rgb * light.colorIntensity.w;

        if (type == LIGHT_TYPE_DIRECTIONAL) {
            // directionType.xyz is the light emission direction. Shading uses the surface-to-light vector.
            L = normalize(-light.directionType.xyz);
            if (directionalSeen == 0u) {
                visibility = sampleDirectionalShadow(worldPos, N, L);
                visibility = min(visibility, texture(contactShadowTexture, inUV).r);
            }
            directionalSeen++;
        } else {
            vec3 toLight = light.positionRange.xyz - worldPos;
            float distanceToLight = length(toLight);
            L = toLight / max(distanceToLight, 0.0001);

            float attenuation = 1.0 / max(distanceToLight * distanceToLight, 1.0);
            float range = light.positionRange.w;
            if (range > 0.0) {
                float rangeFade = clamp(1.0 - distanceToLight / range, 0.0, 1.0);
                attenuation *= rangeFade * rangeFade;
            }

            if (type == LIGHT_TYPE_SPOT) {
                vec3 lightToSurface = normalize(worldPos - light.positionRange.xyz);
                float spotCos = dot(lightToSurface, normalize(light.directionType.xyz));
                float innerCos = light.params.x;
                float outerCos = light.params.y;
                float spotAttenuation = clamp((spotCos - outerCos) / max(innerCos - outerCos, 0.0001), 0.0, 1.0);
                attenuation *= spotAttenuation * spotAttenuation;
            }

            radiance *= attenuation;
        }

        Lo += evaluatePBRDirect(albedo, metallic, roughness, N, V, L, radiance*visibility);
    }

    // --- Ambient Lighting ---
    vec3 ambient = lightData.ambientColor.rgb * albedo * ao;

    vec3 color = ambient + Lo;

    // --- HDR & Gamma Correction (if not using sRGB output target) ---
    // color = color / (color + vec3(1.0)); // simple tone mapping
    // color = pow(color, vec3(1.0/2.2)); 

    outFragColor = vec4(color, 1.0);
}
