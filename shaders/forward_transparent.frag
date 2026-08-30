#version 450

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#define USE_LIGHT_DATA 1
#include "input_structures.glsl"

layout (location = 0) in vec3 inWorldPos;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec2 inUV;
layout (location = 3) in vec4 inColor;
layout (location = 4) flat in uint inMaterialID;

layout (location = 0) out vec4 outFragColor;

layout(set = 3, binding = 0) uniform sampler2D shadowMap;
layout(set = 3, binding = 2) uniform sampler2D punctualShadowMap;

const int SHADOW_CASCADE_COUNT = 4;
const float PI = 3.14159265359;

layout(set = 3, binding = 1) uniform ShadowDataBuffer {
    mat4 lightViewProj[SHADOW_CASCADE_COUNT];
    vec4 cascadeSplits;
    vec4 cascadeBlendWidths;
    vec4 pcfKernelRadii;
    vec4 cascadeTexelWorldSizes;
    vec4 cascadeDepthRanges;
    vec4 lightDir;
    vec4 params;
} shadowData;

const int MAX_PUNCTUAL_SHADOWS = 16;
const int MAX_PUNCTUAL_SHADOW_FACES = 6;

struct PunctualShadow {
    mat4 lightViewProj[MAX_PUNCTUAL_SHADOW_FACES];
    vec4 atlasScaleOffset[MAX_PUNCTUAL_SHADOW_FACES];
    vec4 positionRange;
    vec4 params;
};

layout(std140, set = 3, binding = 3) uniform PunctualShadowDataBuffer {
    uvec4 meta;
    PunctualShadow shadows[MAX_PUNCTUAL_SHADOWS];
} punctualShadowData;

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

    if (lightClip.w <= 0.0
        || any(lessThan(localShadowUV, vec2(0.0)))
        || any(greaterThan(localShadowUV, vec2(1.0)))
        || receiverDepth < 0.0
        || receiverDepth > 1.0) {
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
            vec2 localSampleUV = clamp(
                localShadowUV + vec2(x, y) * texelSize,
                vec2(texelSize * 0.5),
                vec2(1.0 - texelSize * 0.5));
            vec2 atlasUV = atlasOffset + localSampleUV * 0.5;
            float closestDepth = texture(shadowMap, atlasUV).r;
            visibility += (receiverDepth + bias < closestDepth) ? 0.0 : 1.0;
        }
    }

    float sampleWidth = float(kernelRadius * 2 + 1);
    float pcfVisibility = visibility / (sampleWidth * sampleWidth);
    return mix(1.0, pcfVisibility, clamp(shadowData.params.y, 0.0, 1.0));
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

int selectPointShadowFace(vec3 lightToSurface)
{
    vec3 axis = abs(lightToSurface);
    if (axis.x >= axis.y && axis.x >= axis.z) {
        return lightToSurface.x >= 0.0 ? 0 : 1;
    }
    if (axis.y >= axis.z) {
        return lightToSurface.y >= 0.0 ? 2 : 3;
    }
    return lightToSurface.z >= 0.0 ? 4 : 5;
}

float samplePunctualShadow(GPULight light, uint type, vec3 worldPos, vec3 N, vec3 L)
{
    if (light.params.z < 0.0 || light.params.w <= 0.0) {
        return 1.0;
    }

    int shadowIndex = int(light.params.z);
    if (shadowIndex < 0 || shadowIndex >= int(punctualShadowData.meta.x)) {
        return 1.0;
    }

    PunctualShadow shadow = punctualShadowData.shadows[shadowIndex];
    if (shadow.params.w <= 0.0) {
        return 1.0;
    }

    vec3 lightToSurface = worldPos - light.positionRange.xyz;
    float lightDistance = length(lightToSurface);
    if (lightDistance >= shadow.positionRange.w || lightDistance <= 1e-5) {
        return 1.0;
    }

    int faceIndex = type == LIGHT_TYPE_POINT
        ? selectPointShadowFace(lightToSurface)
        : 0;
    float ndotl = max(dot(N, L), 0.0);
    vec3 biasedWorldPos = worldPos
        + N * shadow.params.y * mix(2.0, 1.0, ndotl);
    vec4 lightClip = shadow.lightViewProj[faceIndex] * vec4(biasedWorldPos, 1.0);
    vec3 lightNdc = lightClip.xyz / lightClip.w;
    vec2 localUV = lightNdc.xy * 0.5 + 0.5;
    float receiverDepth = lightNdc.z;
    if (lightClip.w <= 0.0
        || any(lessThan(localUV, vec2(0.0)))
        || any(greaterThan(localUV, vec2(1.0)))
        || receiverDepth < 0.0
        || receiverDepth > 1.0) {
        return 1.0;
    }

    float localTexelSize = 1.0 / max(float(punctualShadowData.meta.y), 1.0);
    vec4 atlasTransform = shadow.atlasScaleOffset[faceIndex];
    float visibility = 0.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 sampleLocalUV = clamp(
                localUV + vec2(x, y) * localTexelSize,
                vec2(localTexelSize * 0.5),
                vec2(1.0 - localTexelSize * 0.5));
            vec2 atlasUV = atlasTransform.zw + sampleLocalUV * atlasTransform.xy;
            float closestDepth = texture(punctualShadowMap, atlasUV).r;
            visibility += receiverDepth < closestDepth ? 0.0 : 1.0;
        }
    }

    float pcfVisibility = visibility / 9.0;
    return mix(1.0, pcfVisibility, clamp(shadow.params.z, 0.0, 1.0));
}

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float distributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float denominator = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denominator * denominator, 0.0001);
}

float geometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 0.0001);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    return geometrySchlickGGX(max(dot(N, V), 0.0), roughness)
        * geometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

vec3 evaluatePBRDirect(
    vec3 albedo,
    float metallic,
    float roughness,
    vec3 N,
    vec3 V,
    vec3 L,
    vec3 radiance)
{
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) {
        return vec3(0.0);
    }

    vec3 H = normalize(V + L);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    float D = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3 specular = (D * G * F)
        / max(4.0 * max(dot(N, V), 0.0) * NdotL, 0.0001);

    vec3 diffuseWeight = (vec3(1.0) - F) * (1.0 - metallic);
    return (diffuseWeight * albedo / PI + specular) * radiance * NdotL;
}

#include "area_light.glsl"

void main()
{
    MaterialData mat = materials[inMaterialID];
    vec4 baseColor = texture(globalTextures[nonuniformEXT(mat.colorTexID)], inUV)
        * mat.colorFactors;
    float alpha = clamp(baseColor.a, 0.0, 1.0);
    if (alpha <= 0.001) {
        discard;
    }

    vec4 metalRoughSample = texture(
        globalTextures[nonuniformEXT(mat.metalRoughTexID)],
        inUV);
    float metallic = clamp(metalRoughSample.b * mat.metal_rough_factors.x, 0.0, 1.0);
    float roughness = clamp(metalRoughSample.g * mat.metal_rough_factors.y, 0.04, 1.0);
    float ao = clamp(texture(globalTextures[nonuniformEXT(mat.occlusionTexID)], inUV).r, 0.0, 1.0);

    vec3 N = normalize(inNormal);
    if (!gl_FrontFacing) {
        N = -N;
    }

    vec3 cameraPosition = inverse(sceneData.view)[3].xyz;
    vec3 V = normalize(cameraPosition - inWorldPos);
    vec3 lighting = vec3(0.0);
    uint directionalSeen = 0;

    for (uint i = 0; i < lightData.lightCount; i++) {
        GPULight light = lights[i];
        uint type = uint(light.directionType.w + 0.5);

        if (type == LIGHT_TYPE_RECT_AREA) {
            lighting += evaluateRectAreaLight(
                baseColor.rgb,
                metallic,
                roughness,
                N,
                V,
                inWorldPos,
                light);
            continue;
        }

        vec3 L;
        vec3 radiance = light.colorIntensity.rgb * light.colorIntensity.w;
        float visibility = 1.0;

        if (type == LIGHT_TYPE_DIRECTIONAL) {
            L = normalize(-light.directionType.xyz);
            if (directionalSeen == 0u) {
                visibility = sampleDirectionalShadow(inWorldPos, N, L);
            }
            directionalSeen++;
        } else {
            vec3 toLight = light.positionRange.xyz - inWorldPos;
            float distanceToLight = length(toLight);
            L = toLight / max(distanceToLight, 0.0001);

            float attenuation = 1.0 / max(distanceToLight * distanceToLight, 1.0);
            float range = light.positionRange.w;
            if (range > 0.0) {
                float rangeFade = clamp(1.0 - distanceToLight / range, 0.0, 1.0);
                attenuation *= rangeFade * rangeFade;
            }

            if (type == LIGHT_TYPE_SPOT) {
                vec3 lightToSurface = normalize(inWorldPos - light.positionRange.xyz);
                float spotCos = dot(lightToSurface, normalize(light.directionType.xyz));
                float innerCos = light.params.x;
                float outerCos = light.params.y;
                float spotAttenuation = clamp(
                    (spotCos - outerCos) / max(innerCos - outerCos, 0.0001),
                    0.0,
                    1.0);
                attenuation *= spotAttenuation * spotAttenuation;
            }
            visibility = samplePunctualShadow(light, type, inWorldPos, N, L);
            radiance *= attenuation;
        }

        lighting += evaluatePBRDirect(
            baseColor.rgb,
            metallic,
            roughness,
            N,
            V,
            L,
            radiance * visibility);
    }

    lighting += lightData.ambientColor.rgb * baseColor.rgb * ao;
    outFragColor = vec4(lighting, alpha);
}
