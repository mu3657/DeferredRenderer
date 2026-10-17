#ifndef ENVIRONMENT_MAP_GLSL
#define ENVIRONMENT_MAP_GLSL

layout(std140, set = ENVIRONMENT_SET, binding = 3) uniform EnvironmentParameters {
    vec4 intensity;
} environmentParameters;

float environmentIntensity() {
    return environmentParameters.intensity.x;
}

float environmentLightingIntensity() {
    return environmentParameters.intensity.x * environmentParameters.intensity.y;
}

vec2 environmentPanoramaUV(vec3 direction) {
    direction = normalize(direction);
    const float inversePi = 0.3183098861837907;
    return vec2(
        atan(direction.z, direction.x) * (0.5 * inversePi) + 0.5,
        acos(clamp(direction.y, -1.0, 1.0)) * inversePi);
}

vec3 sampleEnvironmentPanorama(sampler2D panorama, vec3 direction) {
    return textureLod(panorama, environmentPanoramaUV(direction), 0.0).rgb;
}

vec3 sampleEnvironmentSpecular(samplerCube filteredMap, sampler2D panorama,
    vec3 direction, float roughness, float lastMip) {
    float r = clamp(roughness, 0.0, 1.0);
    vec3 filtered = textureLod(filteredMap, direction, r * lastMip).rgb;
    vec3 sharp = sampleEnvironmentPanorama(panorama, direction);
    return mix(sharp, filtered, smoothstep(0.08, 0.22, r)) * environmentIntensity();
}

#endif
