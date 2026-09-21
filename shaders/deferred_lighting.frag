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
layout(set = 1, binding = 4) uniform sampler2D gEmissive;
layout(set = 3, binding = 0) uniform sampler2D shadowMap;
layout(set = 3, binding = 2) uniform sampler2D punctualShadowMap;
layout(set = 4, binding = 0) uniform sampler2D contactShadowTexture;

layout(push_constant) uniform DeferredLightingPushConstants {
    vec4 ddgiParams;      // x history valid, y intensity, z debug mode, w composite enabled
    vec4 ddgiDebugParams; // x DDGI heatmap exposure, y PBR debug mode, zw unused
    uvec4 lightGridParams; // xy tile counts, z tile size, w flags
} lightingPushConstants;

layout(std140, set = 5, binding = 0) uniform DDGIVolumeConstants {
    vec4 originMaxRayDistance;
    vec4 spacingHysteresis;
    uvec4 probeCountsRaysPerProbe;
    vec4 biasAndEncoding;
    vec4 blendThresholds;
    uvec4 texelsAndUpdate;
    ivec4 scrollOffsets;
    vec4 rayRotation;
    uvec4 frameAndFlags;
} ddgiVolume;

layout(set = 5, binding = 1) uniform sampler2DArray ddgiIrradianceAtlas;
layout(set = 5, binding = 2) uniform sampler2DArray ddgiDistanceAtlas;
layout(set = 5, binding = 3) uniform sampler2DArray ddgiProbeData;

const uint DDGI_VOLUME_FLAG_RELOCATION = 1u << 1;

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

const int MAX_PUNCTUAL_SHADOWS = 16;
const int MAX_PUNCTUAL_SHADOW_FACES = 6;

struct PunctualShadow {
    mat4 lightViewProj[MAX_PUNCTUAL_SHADOW_FACES];
    vec4 atlasScaleOffset[MAX_PUNCTUAL_SHADOW_FACES];
    vec4 positionRange;
    vec4 params;
};

layout(std140, set = 3, binding = 3) uniform PunctualShadowDataBuffer {
    uvec4 meta; // x shadow count, y tile resolution, z/w atlas extent
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

#include "pbr.glsl"
#include "area_light.glsl"

// Reconstruct world position from depth
vec3 reconstructPosition(vec2 uv, float depth, mat4 invViewProj) {
    vec4 clipSpace = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 worldSpace = invViewProj * clipSpace;
    return worldSpace.xyz / worldSpace.w;
}

vec3 ddgiHeatmap(float luminance, float exposure)
{
    float exposedLuminance = max(luminance, 0.0) * max(exposure, 0.0);
    if (exposedLuminance <= 1e-6) {
        return vec3(0.0);
    }

    float value = clamp(log2(1.0 + exposedLuminance) / 4.0, 0.0, 1.0);
    return vec3(
        clamp(1.5 - abs(4.0 * value - 3.0), 0.0, 1.0),
        clamp(1.5 - abs(4.0 * value - 2.0), 0.0, 1.0),
        clamp(1.5 - abs(4.0 * value - 1.0), 0.0, 1.0));
}

vec2 signNotZero(vec2 value)
{
    return vec2(value.x >= 0.0 ? 1.0 : -1.0,
                value.y >= 0.0 ? 1.0 : -1.0);
}

vec2 octahedralEncode(vec3 direction)
{
    direction /= max(abs(direction.x) + abs(direction.y) + abs(direction.z), 1e-6);
    vec2 encoded = direction.xy;
    if (direction.z < 0.0) {
        encoded = (vec2(1.0) - abs(encoded.yx)) * signNotZero(encoded);
    }
    return encoded;
}

uint ddgiProbeIndex(uvec3 coordinates)
{
    uvec3 counts = ddgiVolume.probeCountsRaysPerProbe.xyz;
    return coordinates.x
        + coordinates.y * counts.x
        + coordinates.z * counts.x * counts.y;
}

vec3 ddgiProbeWorldPosition(uvec3 coordinates)
{
    ivec3 scrolledCoordinates = ivec3(coordinates) + ddgiVolume.scrollOffsets.xyz;
    vec3 position = ddgiVolume.originMaxRayDistance.xyz
        + vec3(scrolledCoordinates) * ddgiVolume.spacingHysteresis.xyz;
    if ((ddgiVolume.frameAndFlags.y & DDGI_VOLUME_FLAG_RELOCATION) != 0u) {
        position += texelFetch(
            ddgiProbeData,
            ivec3(coordinates.x, coordinates.z, coordinates.y),
            0).xyz;
    }
    return position;
}

uvec3 ddgiProbeTextureCoordinates(uint probeIndex)
{
    uvec3 counts = ddgiVolume.probeCountsRaysPerProbe.xyz;
    uvec3 coordinates = uvec3(
        probeIndex % counts.x,
        (probeIndex / counts.x) % counts.y,
        probeIndex / (counts.x * counts.y));
    return uvec3(coordinates.x, coordinates.z, coordinates.y);
}

vec3 ddgiAtlasUV(
    uint probeIndex,
    vec3 direction,
    uint interiorTexels,
    ivec3 atlasSize)
{
    uint tileTexels = interiorTexels + 2u;
    uvec3 textureCoordinates = ddgiProbeTextureCoordinates(probeIndex);
    uvec2 tileCoordinates = textureCoordinates.xy;
    vec2 octahedral = octahedralEncode(normalize(direction));
    vec2 atlasTexel = vec2(tileCoordinates * tileTexels)
        + vec2(1.0)
        + (octahedral * 0.5 + 0.5) * float(interiorTexels);
    return vec3(atlasTexel / vec2(atlasSize.xy), float(textureCoordinates.z));
}

vec3 sampleDDGIIrradiance(uint probeIndex, vec3 normal)
{
    vec3 uv = ddgiAtlasUV(
        probeIndex,
        normal,
        ddgiVolume.texelsAndUpdate.x,
        textureSize(ddgiIrradianceAtlas, 0));
    vec3 encoded = max(texture(ddgiIrradianceAtlas, uv).rgb, vec3(0.0));
    float encodingGamma = max(ddgiVolume.biasAndEncoding.z, 1e-3);
    // RTXGI leaves a gamma=2 curve in place so trilinear interpolation happens
    // in a perceptually smoother space. The square and 2PI estimator factor
    // are applied after the eight probe samples have been accumulated.
    return pow(encoded, vec3(encodingGamma * 0.5));
}

vec2 sampleDDGIVisibility(
    uint probeIndex,
    vec3 probePosition,
    vec3 surfacePosition)
{
    vec3 probeToSurface = surfacePosition - probePosition;
    float surfaceDistance = length(probeToSurface);
    if (surfaceDistance <= 1e-5) {
        return vec2(1.0, 1.0);
    }

    vec3 uv = ddgiAtlasUV(
        probeIndex,
        probeToSurface,
        ddgiVolume.texelsAndUpdate.y,
        textureSize(ddgiDistanceAtlas, 0));
    // ProbeBlendingCS normalizes distance by 2 to share the irradiance path.
    vec2 moments = 2.0 * texture(ddgiDistanceAtlas, uv).rg;
    if (moments.x <= 0.0 || any(isnan(moments)) || any(isinf(moments))) {
        return vec2(0.0, 0.0);
    }
    if (surfaceDistance <= moments.x) {
        return vec2(1.0, 1.0);
    }

    float variance = max(moments.y - moments.x * moments.x, 1e-5);
    float delta = surfaceDistance - moments.x;
    float chebyshev = variance / (variance + delta * delta);
    return vec2(pow(clamp(chebyshev, 0.0, 1.0), 3.0), 1.0);
}

struct DDGISample {
    vec3 irradiance;
    float confidence;
    float insideVolume;
};

DDGISample sampleDDGI(vec3 worldPosition, vec3 normal, vec3 viewDirection)
{
    DDGISample result = DDGISample(vec3(0.0), 0.0, 0.0);
    vec3 biasedPosition = worldPosition
        + normal * ddgiVolume.biasAndEncoding.x
        + viewDirection * ddgiVolume.biasAndEncoding.y;
    vec3 probeCoordinates =
        (biasedPosition - ddgiVolume.originMaxRayDistance.xyz)
        / ddgiVolume.spacingHysteresis.xyz
        - vec3(ddgiVolume.scrollOffsets.xyz);
    vec3 maximumCoordinates = vec3(ddgiVolume.probeCountsRaysPerProbe.xyz - uvec3(1u));
    if (any(lessThan(probeCoordinates, vec3(-0.5)))
        || any(greaterThan(probeCoordinates, maximumCoordinates + vec3(0.5)))) {
        return result;
    }
    result.insideVolume = 1.0;

    probeCoordinates = clamp(probeCoordinates, vec3(0.0), maximumCoordinates);
    ivec3 baseCoordinates = ivec3(floor(probeCoordinates));
    vec3 alpha = fract(probeCoordinates);
    float irradianceWeight = 0.0;
    float validWeight = 0.0;
    float trilinearWeightSum = 0.0;

    for (uint corner = 0u; corner < 8u; ++corner) {
        ivec3 offset = ivec3(
            int(corner & 1u),
            int((corner >> 1u) & 1u),
            int((corner >> 2u) & 1u));
        ivec3 coordinates = clamp(
            baseCoordinates + offset,
            ivec3(0),
            ivec3(ddgiVolume.probeCountsRaysPerProbe.xyz) - ivec3(1));

        vec3 cornerWeight = max(
            vec3(0.001),
            mix(vec3(1.0) - alpha, alpha, vec3(offset)));
        float trilinearWeight = cornerWeight.x * cornerWeight.y * cornerWeight.z;

        uvec3 probeCoordinatesU = uvec3(coordinates);
        uint probeIndex = ddgiProbeIndex(probeCoordinatesU);
        vec3 probePosition = ddgiProbeWorldPosition(probeCoordinatesU);
        vec2 visibilityAndValidity = sampleDDGIVisibility(
            probeIndex, probePosition, biasedPosition);
        vec3 worldPositionToProbe = normalize(probePosition - worldPosition);
        float wrapShading = (dot(worldPositionToProbe, normal) + 1.0) * 0.5;
        float sampleWeight = (wrapShading * wrapShading) + 0.2;
        sampleWeight *= max(0.05, visibilityAndValidity.x);
        sampleWeight = max(sampleWeight, 1e-6);
        if (sampleWeight < 0.2) {
            sampleWeight *= sampleWeight * sampleWeight * 25.0;
        }
        sampleWeight *= trilinearWeight;
        result.irradiance += sampleDDGIIrradiance(probeIndex, normal) * sampleWeight;
        irradianceWeight += sampleWeight;
        validWeight += trilinearWeight * visibilityAndValidity.y;
        trilinearWeightSum += trilinearWeight;
    }

    if (irradianceWeight > 1e-5) {
        result.irradiance /= irradianceWeight;
        result.irradiance *= result.irradiance;
        result.irradiance *= 2.0 * PI;
    }
    result.confidence = validWeight / max(trilinearWeightSum, 1e-5);
    return result;
}

void accumulateDirectLight(
    uint lightIndex,
    vec3 albedo,
    float metallic,
    float roughness,
    vec3 N,
    vec3 V,
    vec3 worldPos,
    vec2 screenUV,
    inout vec3 Lo,
    inout uint directionalSeen)
{
    float visibility = 1.0;
    GPULight light = lights[lightIndex];
    uint type = uint(light.directionType.w + 0.5);

    if (type == LIGHT_TYPE_RECT_AREA) {
        Lo += evaluateRectAreaLight(albedo, metallic, roughness, N, V, worldPos, light);
        return;
    }

    vec3 L = vec3(0.0);
    vec3 radiance = light.colorIntensity.rgb * light.colorIntensity.w;

    if (type == LIGHT_TYPE_DIRECTIONAL) {
        // directionType.xyz is the light emission direction. Shading uses the surface-to-light vector.
        L = normalize(-light.directionType.xyz);
        if (directionalSeen == 0u) {
            visibility = sampleDirectionalShadow(worldPos, N, L);
            visibility = min(visibility, texture(contactShadowTexture, screenUV).r);
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
            float spotAttenuation = clamp(
                (spotCos - outerCos) / max(innerCos - outerCos, 0.0001),
                0.0,
                1.0);
            attenuation *= spotAttenuation * spotAttenuation;
        }

        visibility = samplePunctualShadow(light, type, worldPos, N, L);
        radiance *= attenuation;
    }

    Lo += evaluatePBRDirect(
        albedo, metallic, roughness, N, V, L, radiance * visibility);
}

vec3 lightCountHeatmap(float normalizedCount)
{
    float value = clamp(normalizedCount, 0.0, 1.0);
    return vec3(
        clamp(1.5 - abs(4.0 * value - 3.0), 0.0, 1.0),
        clamp(1.5 - abs(4.0 * value - 2.0), 0.0, 1.0),
        clamp(1.5 - abs(4.0 * value - 1.0), 0.0, 1.0));
}

void main()
{
    // 1. Sample G-Buffer
    vec4 albedoSample   = texture(gAlbedo, inUV);
    vec4 normalSample   = texture(gNormal, inUV); // Normal encoded as 0..1, a=metallic
    vec4 materialSample = texture(gMaterial, inUV); // r=roughness, g=ao
    vec3 emissive       = max(texture(gEmissive, inUV).rgb, vec3(0.0));
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
    float NdotV = max(dot(N, V), 0.0);

    vec3 Lo = vec3(0.0);

    uint directionalSeen = 0;
    uint evaluatedLightCount = 0;
    // --- Direct Lighting ---
    bool tiledLightingEnabled = (lightingPushConstants.lightGridParams.w & 1u) != 0u;
    if (tiledLightingEnabled) {
        uint tileSize = max(lightingPushConstants.lightGridParams.z, 1u);
        uvec2 tile = uvec2(gl_FragCoord.xy) / tileSize;
        tile = min(tile, lightingPushConstants.lightGridParams.xy - uvec2(1u));
        uint tileIndex = tile.y * lightingPushConstants.lightGridParams.x + tile.x;

        for (uint wordIndex = 0u; wordIndex < 8u; ++wordIndex) {
            uint mask = tileLightMasks[tileIndex * 8u + wordIndex];
            evaluatedLightCount += bitCount(mask);
            while (mask != 0u) {
                uint bitIndex = uint(findLSB(mask));
                uint lightIndex = wordIndex * 32u + bitIndex;
                if (lightIndex < lightData.lightCount) {
                    accumulateDirectLight(
                        lightIndex,
                        albedo,
                        metallic,
                        roughness,
                        N,
                        V,
                        worldPos,
                        inUV,
                        Lo,
                        directionalSeen);
                }
                mask &= mask - 1u;
            }
        }
    } else {
        evaluatedLightCount = lightData.lightCount;
        for (uint lightIndex = 0u; lightIndex < lightData.lightCount; ++lightIndex) {
            accumulateDirectLight(
                lightIndex,
                albedo,
                metallic,
                roughness,
                N,
                V,
                worldPos,
                inUV,
                Lo,
                directionalSeen);
        }
    }

    // --- Ambient and DDGI diffuse indirect lighting ---
    vec3 diffuseEnergy = pbrDiffuseEnergy(albedo, metallic, roughness, NdotV);
    vec3 legacyAmbient = lightData.ambientColor.rgb * albedo * diffuseEnergy * ao;
    DDGISample ddgi = DDGISample(vec3(0.0), 0.0, 0.0);
    vec3 indirectDiffuse = vec3(0.0);
    if (lightingPushConstants.ddgiParams.x > 0.5) {
        ddgi = sampleDDGI(worldPos, N, V);
        indirectDiffuse = evaluatePBRDiffuseIrradiance(
            ddgi.irradiance,
            albedo,
            metallic,
            roughness,
            NdotV)
            * ao
            * lightingPushConstants.ddgiParams.y;
    }
    vec3 ambient = lightingPushConstants.ddgiParams.w > 0.5
        ? mix(legacyAmbient, indirectDiffuse, ddgi.confidence)
        : legacyAmbient;

    vec3 emissiveContribution = lightingPushConstants.ddgiDebugParams.z > 0.5
        ? emissive
        : vec3(0.0);
    vec3 color = emissiveContribution + ambient + Lo;

    int ddgiDebugMode = int(lightingPushConstants.ddgiParams.z + 0.5);
    if (ddgiDebugMode == 1) {
        color = indirectDiffuse;
    } else if (ddgiDebugMode == 2) {
        color = ddgi.insideVolume < 0.5
            ? vec3(1.0, 0.0, 1.0)
            : mix(vec3(1.0, 0.05, 0.0), vec3(0.0, 1.0, 0.1), ddgi.confidence);
    } else if (ddgiDebugMode == 3) {
        color = ddgiHeatmap(
            dot(ddgi.irradiance, vec3(0.2126, 0.7152, 0.0722)),
            lightingPushConstants.ddgiDebugParams.x);
    } else if (ddgiDebugMode == 4) {
        color = ddgiHeatmap(
            dot(indirectDiffuse, vec3(0.2126, 0.7152, 0.0722)),
            lightingPushConstants.ddgiDebugParams.x);
    }

    if ((lightingPushConstants.lightGridParams.w & 2u) != 0u) {
        float normalizedLightCount = float(evaluatedLightCount)
            / max(float(lightData.lightCount), 1.0);
        color = lightCountHeatmap(normalizedLightCount);
    }

    int pbrDebugMode = int(lightingPushConstants.ddgiDebugParams.y + 0.5);
    if (pbrDebugMode == 1) {
        color = albedo;
    } else if (pbrDebugMode == 2) {
        color = N * 0.5 + 0.5;
    } else if (pbrDebugMode == 3) {
        color = vec3(metallic);
    } else if (pbrDebugMode == 4) {
        color = vec3(roughness);
    } else if (pbrDebugMode == 5) {
        color = vec3(ao);
    } else if (pbrDebugMode == 6) {
        color = emissive / (emissive + vec3(1.0));
    } else if (pbrDebugMode == 7) {
        // Red marks normals rejected by the direct BRDF's view hemisphere.
        color = dot(N, V) <= 0.0 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    } else if (pbrDebugMode == 8) {
        color = Lo;
    }

    // --- HDR & Gamma Correction (if not using sRGB output target) ---
    // color = color / (color + vec3(1.0)); // simple tone mapping
    // color = pow(color, vec3(1.0/2.2)); 

    outFragColor = vec4(color, 1.0);
}
