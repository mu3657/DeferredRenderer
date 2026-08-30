#version 460

layout(location = 0) in vec2 inCircleCoordinates;
layout(location = 1) flat in vec3 inCameraRight;
layout(location = 2) flat in vec3 inCameraUp;
layout(location = 3) flat in vec3 inCameraForward;
layout(location = 4) flat in uint inProbeIndex;
layout(location = 0) out vec4 outFragColor;

layout(std140, set = 1, binding = 0) uniform DDGIVolumeConstants {
    vec4 originMaxRayDistance;
    vec4 spacingHysteresis;
    uvec4 probeCountsRaysPerProbe;
    vec4 biasAndEncoding;
    vec4 blendThresholds;
    uvec4 texelsAndUpdate;
    ivec4 scrollOffsets;
    vec4 rayRotation;
    uvec4 frameAndFlags;
} volume;

layout(set = 1, binding = 1) uniform sampler2DArray irradianceAtlas;
layout(set = 1, binding = 2) uniform sampler2DArray distanceAtlas;

layout(push_constant) uniform DDGIProbeDebugPushConstants {
    vec4 originRadius;
    vec4 spacingIntensity;
    uvec4 countsAndMode;
    uvec4 debugParams;
} pushConstants;

const uint MODE_POSITION = 0u;
const uint MODE_IRRADIANCE = 1u;
const uint MODE_UPDATE_STATE = 2u;
const float PI = 3.14159265358979323846;

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

uvec3 probeTextureCoordinates(uint probeIndex)
{
    uvec3 counts = volume.probeCountsRaysPerProbe.xyz;
    uvec3 coordinates = uvec3(
        probeIndex % counts.x,
        (probeIndex / counts.x) % counts.y,
        probeIndex / (counts.x * counts.y));
    return uvec3(coordinates.x, coordinates.z, coordinates.y);
}

vec3 atlasUV(uint probeIndex, vec3 direction, uint interiorTexels, ivec3 atlasSize)
{
    uint tileTexels = interiorTexels + 2u;
    uvec3 textureCoordinates = probeTextureCoordinates(probeIndex);
    uvec2 tileCoordinates = textureCoordinates.xy;
    vec2 atlasTexel = vec2(tileCoordinates * tileTexels)
        + vec2(1.0)
        + (octahedralEncode(normalize(direction)) * 0.5 + 0.5)
            * float(interiorTexels);
    return vec3(atlasTexel / vec2(atlasSize.xy), float(textureCoordinates.z));
}

bool probeHasHistory(uint probeIndex, vec3 direction)
{
    vec3 uv = atlasUV(
        probeIndex,
        direction,
        volume.texelsAndUpdate.y,
        textureSize(distanceAtlas, 0));
    return texture(distanceAtlas, uv).r > 0.0;
}

void main()
{
    float radiusSquared = dot(inCircleCoordinates, inCircleCoordinates);
    if (radiusSquared > 1.0) {
        discard;
    }
    float sphereDepth = sqrt(max(1.0 - radiusSquared, 0.0));
    vec3 normal = normalize(
        inCameraRight * inCircleCoordinates.x
        + inCameraUp * inCircleCoordinates.y
        + inCameraForward * sphereDepth);
    vec3 color;

    if (pushConstants.countsAndMode.w == MODE_IRRADIANCE) {
        vec3 uv = atlasUV(
            inProbeIndex,
            normal,
            volume.texelsAndUpdate.x,
            textureSize(irradianceAtlas, 0));
        vec3 encoded = max(texture(irradianceAtlas, uv).rgb, vec3(0.0));
        float encodingGamma = max(volume.biasAndEncoding.z, 1e-3);
        color = pow(encoded, vec3(encodingGamma))
            * (2.0 * PI * pushConstants.spacingIntensity.w);
    } else if (pushConstants.countsAndMode.w == MODE_UPDATE_STATE) {
        uint updateBegin = volume.texelsAndUpdate.z;
        uint updateEnd = updateBegin + volume.texelsAndUpdate.w;
        bool currentBatch = inProbeIndex >= updateBegin && inProbeIndex < updateEnd;
        if (currentBatch) {
            color = vec3(1.0, 0.72, 0.05);
        } else if (probeHasHistory(inProbeIndex, normal)) {
            color = vec3(0.08, 0.85, 0.25);
        } else {
            color = vec3(0.16);
        }
    } else {
        color = vec3(0.08, 0.65, 1.0);
    }

    // A small amount of directional shading preserves the sphere silhouette.
    float shape = 0.35 + 0.65 * sphereDepth;
    outFragColor = vec4(max(color * shape, vec3(0.0)), 1.0);
}
