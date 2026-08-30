#version 460

layout(location = 0) out vec2 outCircleCoordinates;
layout(location = 1) flat out vec3 outCameraRight;
layout(location = 2) flat out vec3 outCameraUp;
layout(location = 3) flat out vec3 outCameraForward;
layout(location = 4) flat out uint outProbeIndex;

layout(std140, set = 0, binding = 0) uniform SceneData {
    mat4 view;
    mat4 proj;
    mat4 viewproj;
    vec4 ambientColor;
    vec4 sunlightDirection;
    vec4 sunlightColor;
} sceneData;

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

layout(push_constant) uniform DDGIProbeDebugPushConstants {
    vec4 originRadius;
    vec4 spacingIntensity;
    uvec4 countsAndMode;
    uvec4 debugParams;
} pushConstants;

const vec2 QUAD_CORNERS[6] = vec2[6](
    vec2(-1.0, -1.0),
    vec2( 1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0,  1.0));

uvec3 probeCoordinates(uint probeIndex)
{
    uvec3 counts = pushConstants.countsAndMode.xyz;
    uint planeSize = counts.x * counts.y;
    return uvec3(
        probeIndex % counts.x,
        (probeIndex / counts.x) % counts.y,
        probeIndex / planeSize);
}

void main()
{
    uint probeIndex = uint(gl_VertexIndex) / 6u;
    uint cornerIndex = uint(gl_VertexIndex) % 6u;
    vec2 corner = QUAD_CORNERS[cornerIndex];
    if (pushConstants.debugParams.x == 1u) {
        outCircleCoordinates = corner;
        outCameraRight = vec3(1.0, 0.0, 0.0);
        outCameraUp = vec3(0.0, 1.0, 0.0);
        outCameraForward = vec3(0.0, 0.0, 1.0);
        outProbeIndex = 0u;
        gl_Position = vec4(corner * 0.12, 0.5, 1.0);
        return;
    }

    mat3 cameraToWorld = mat3(inverse(sceneData.view));
    vec3 cameraRight = normalize(cameraToWorld[0]);
    vec3 cameraUp = normalize(cameraToWorld[1]);
    vec3 cameraForward = normalize(cameraToWorld[2]);
    vec3 probePosition;
    if (pushConstants.debugParams.x == 2u) {
        vec3 cameraPosition = inverse(sceneData.view)[3].xyz;
        probePosition = cameraPosition - cameraForward * 2.0;
        probeIndex = 0u;
    } else {
        uvec3 coordinates = probeCoordinates(probeIndex);
        probePosition = pushConstants.originRadius.xyz
            + vec3(coordinates) * pushConstants.spacingIntensity.xyz;
    }
    vec3 worldPosition = probePosition
        + (cameraRight * corner.x + cameraUp * corner.y) * pushConstants.originRadius.w;

    outCircleCoordinates = corner;
    outCameraRight = cameraRight;
    outCameraUp = cameraUp;
    outCameraForward = cameraForward;
    outProbeIndex = probeIndex;
    gl_Position = sceneData.viewproj * vec4(worldPosition, 1.0);
}
