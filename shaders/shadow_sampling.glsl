#ifndef DIRECTIONAL_SHADOW_SAMPLING_GLSL
#define DIRECTIONAL_SHADOW_SAMPLING_GLSL

// Shared by deferred and forward lighting. All depths use reverse Z.
layout(set = 3, binding = 0) uniform sampler2D shadowMap;
layout(set = 3, binding = 4) uniform sampler2DShadow shadowCompareMap;

const int SHADOW_CASCADE_COUNT = 4;
const int MAX_PCSS_SAMPLES = 64;
const vec2 SHADOW_DISK[32] = vec2[](
    vec2(0.1250000, 0.0000000), vec2(-0.1596450, 0.1462479),
    vec2(0.0244362, -0.2784383), vec2(0.2012222, 0.2624588),
    vec2(-0.3692676, -0.0653182), vec2(0.3498025, -0.2225157),
    vec2(-0.1170021, 0.4352419), vec2(-0.2231357, -0.4296341),
    vec2(0.4841151, 0.1767981), vec2(-0.5036411, 0.2078957),
    vec2(0.2427883, -0.5188245), vec2(0.1794144, 0.5720013),
    vec2(-0.5407570, -0.3133797), vec2(0.6343695, -0.1394644),
    vec2(-0.3871458, 0.5506751), vec2(-0.0894397, -0.6901996),
    vec2(0.5490718, 0.4627583), vec2(-0.7388785, 0.0305549),
    vec2(0.5389551, -0.5363323), vec2(-0.0360582, 0.7797915),
    vec2(-0.5128175, -0.6145268), vec2(0.8123596, 0.1093018),
    vec2(-0.6883106, 0.4789086), vec2(0.1880861, -0.8360614),
    vec2(0.4350333, 0.7591911), vec2(-0.8504484, -0.2713162),
    vec2(0.8261024, -0.3816803), vec2(-0.3578882, 0.8551556),
    vec2(-0.3194073, -0.8880338), vec2(0.8499086, 0.4466882),
    vec2(-0.9440346, 0.2488445), vec2(0.5365958, -0.8345298));

layout(std140, set = 3, binding = 1) uniform ShadowDataBuffer {
    mat4 lightViewProj[SHADOW_CASCADE_COUNT];
    vec4 cascadeSplits;
    vec4 cascadeBlendWidths;
    vec4 pcfKernelRadii;
    vec4 cascadeTexelWorldSizes;
    vec4 cascadeDepthRanges;
    vec4 lightDir;
    vec4 params;       // receiver bias, strength, local texel size, enabled
    vec4 pcssParams;   // enabled, tan(sun half angle), search radius, filter radius
    vec4 pcssSamples;  // blocker count, filter count, minimum filter radius, reserved
} shadowData;

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

vec2 shadowAtlasUV(int cascadeIndex, vec2 localUV)
{
    float halfTexel = shadowData.params.z * 0.5;
    vec2 safeUV = clamp(localUV, vec2(halfTexel), vec2(1.0 - halfTexel));
    return cascadeAtlasOffset(cascadeIndex) + safeUV * 0.5;
}

// Fixed disk samples avoid frame-to-frame stochastic noise.
vec2 shadowDiskOffset(int index, int count)
{
    if (count > 32) {
        float radius = sqrt((float(index) + 0.5) / float(count));
        float angle = float(index) * 2.399963229728653;
        return vec2(cos(angle), sin(angle)) * radius;
    }
    return SHADOW_DISK[index] * sqrt(32.0 / float(count));
}

float shadowCompare(int cascadeIndex, vec2 localUV, float receiverDepth)
{
    return texture(shadowCompareMap, vec3(shadowAtlasUV(cascadeIndex, localUV), receiverDepth));
}

float shadowPCF(int cascadeIndex, vec2 localUV, float receiverDepth,
    float radiusTexels, int sampleCount)
{
    if (radiusTexels <= 0.75) {
        return shadowCompare(cascadeIndex, localUV, receiverDepth);
    }

    float visibility = 0.0;
    for (int i = 0; i < MAX_PCSS_SAMPLES; ++i) {
        if (i >= sampleCount) break;
        vec2 sampleUV = localUV + shadowDiskOffset(i, sampleCount) *
            radiusTexels * shadowData.params.z;
        visibility += shadowCompare(cascadeIndex, sampleUV, receiverDepth);
    }
    return visibility / float(sampleCount);
}

float sampleShadowCascade(int cascadeIndex, vec3 worldPos, vec3 N, vec3 L)
{
    vec4 lightClip = shadowData.lightViewProj[cascadeIndex] * vec4(worldPos, 1.0);
    if (lightClip.w <= 0.0) return 1.0;
    vec3 lightNdc = lightClip.xyz / lightClip.w;
    vec2 localUV = lightNdc.xy * 0.5 + 0.5;
    float receiverDepth = lightNdc.z;
    if (any(lessThan(localUV, vec2(0.0))) ||
        any(greaterThan(localUV, vec2(1.0))) ||
        receiverDepth < 0.0 || receiverDepth > 1.0) return 1.0;

    float ndotl = max(dot(N, L), 0.0);
    float texelWorldSize = max(shadowData.cascadeTexelWorldSizes[cascadeIndex], 1e-5);
    float depthRange = max(shadowData.cascadeDepthRanges[cascadeIndex], 1e-4);
    float bias = texelWorldSize * shadowData.params.x * mix(2.0, 1.0, ndotl) / depthRange;
    float biasedDepth = min(receiverDepth + bias, 1.0);
    float visibility;
    if (shadowData.pcssParams.x > 0.5) {
        // A blocker can be anywhere between the light's near plane and the
        // receiver. Bound the search for stable work and atlas isolation.
        float distanceFromNear = (1.0 - receiverDepth) * depthRange;
        float searchRadius = clamp(
            distanceFromNear * shadowData.pcssParams.y / texelWorldSize,
            1.0, shadowData.pcssParams.z);
        int blockerSamples = clamp(int(shadowData.pcssSamples.x + 0.5), 8, 32);
        int filterSamples = clamp(int(shadowData.pcssSamples.y + 0.5), 8, MAX_PCSS_SAMPLES);
        float blockerDepthSum = 0.0;
        int blockerCount = 0;
        float centerDepth = texture(shadowMap, shadowAtlasUV(cascadeIndex, localUV)).r;
        if (centerDepth > receiverDepth + bias) {
            blockerDepthSum = centerDepth;
            blockerCount = 1;
        }
        int nearSamples = blockerSamples / 2;
        for (int i = 0; i < MAX_PCSS_SAMPLES; ++i) {
            if (i >= blockerSamples) break;
            bool nearRing = i < nearSamples;
            vec2 diskOffset = nearRing
                ? shadowDiskOffset(i, nearSamples)
                : shadowDiskOffset(i - nearSamples, blockerSamples - nearSamples);
            float ringRadius = nearRing ? min(searchRadius, 4.0) : searchRadius;
            vec2 sampleUV = localUV + diskOffset * ringRadius * shadowData.params.z;
            vec2 atlasUV = shadowAtlasUV(cascadeIndex, sampleUV);
            float sampleDepth = texture(shadowMap, atlasUV).r;
            if (sampleDepth > receiverDepth + bias) {
                blockerDepthSum += sampleDepth;
                ++blockerCount;
            }
        }

        if (blockerCount == 0) {
            // Retain a small contact test for narrow casters missed by the
            // sparse search disk.
            visibility = shadowPCF(cascadeIndex, localUV, biasedDepth, 1.0, 8);
        } else {
            float blockerDistance = blockerDepthSum / float(blockerCount) - receiverDepth;
            float blockerReceiverDistance = max(blockerDistance, 0.0) * depthRange;
            float penumbraRadius = clamp(
                blockerReceiverDistance * shadowData.pcssParams.y / texelWorldSize,
                shadowData.pcssSamples.z, shadowData.pcssParams.w);
            visibility = shadowPCF(cascadeIndex, localUV, biasedDepth, penumbraRadius, filterSamples);
        }
    } else {
        float radius = clamp(shadowData.pcfKernelRadii[cascadeIndex], 0.0, 3.0);
        visibility = shadowPCF(cascadeIndex, localUV, biasedDepth, max(radius, 0.75), 16);
    }

    return mix(1.0, visibility, clamp(shadowData.params.y, 0.0, 1.0));
}

float sampleDirectionalShadow(vec3 worldPos, vec3 N, vec3 L)
{
    if (shadowData.params.w <= 0.0) return 1.0;
    float viewDepth = -(sceneData.view * vec4(worldPos, 1.0)).z;
    ShadowCascadeSelection selection = selectShadowCascades(viewDepth);
    if (selection.primary < 0) return 1.0;

    float visibility = sampleShadowCascade(selection.primary, worldPos, N, L);
    if (selection.secondary >= 0) {
        visibility = mix(visibility,
            sampleShadowCascade(selection.secondary, worldPos, N, L),
            selection.blend);
    }
    return visibility;
}

#endif
