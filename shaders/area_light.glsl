const int RECT_AREA_SAMPLE_COUNT = 9;

const vec2 RECT_AREA_SAMPLE_OFFSETS[RECT_AREA_SAMPLE_COUNT] = vec2[](
    vec2(-0.6666667, -0.6666667),
    vec2( 0.0,       -0.6666667),
    vec2( 0.6666667, -0.6666667),
    vec2(-0.6666667,  0.0),
    vec2( 0.0,        0.0),
    vec2( 0.6666667,  0.0),
    vec2(-0.6666667,  0.6666667),
    vec2( 0.0,        0.6666667),
    vec2( 0.6666667,  0.6666667)
);

vec3 evaluateRectAreaLight(
    vec3 albedo,
    float metallic,
    float roughness,
    vec3 N,
    vec3 V,
    vec3 worldPos,
    GPULight light)
{
    vec3 emissionDirection = normalize(light.directionType.xyz);
    vec3 right = normalize(light.areaRight.xyz);
    vec3 up = normalize(light.areaUp.xyz);
    float halfWidth = max(light.areaRight.w, 0.005);
    float halfHeight = max(light.areaUp.w, 0.005);
    float sampleArea = (4.0 * halfWidth * halfHeight) / float(RECT_AREA_SAMPLE_COUNT);
    vec3 emittedRadiance = light.colorIntensity.rgb * light.colorIntensity.w;
    vec3 result = vec3(0.0);

    for (int sampleIndex = 0; sampleIndex < RECT_AREA_SAMPLE_COUNT; sampleIndex++) {
        vec2 offset = RECT_AREA_SAMPLE_OFFSETS[sampleIndex];
        vec3 samplePosition = light.positionRange.xyz
            + right * (offset.x * halfWidth)
            + up * (offset.y * halfHeight);
        vec3 toLight = samplePosition - worldPos;
        float distanceSquared = dot(toLight, toLight);
        float distanceToLight = sqrt(max(distanceSquared, 0.0001));
        vec3 L = toLight / distanceToLight;

        float emitterCosine = max(dot(emissionDirection, -L), 0.0);
        if (emitterCosine <= 0.0) {
            continue;
        }

        float rangeFade = 1.0;
        if (light.positionRange.w > 0.0) {
            rangeFade = clamp(1.0 - distanceToLight / light.positionRange.w, 0.0, 1.0);
            rangeFade *= rangeFade;
        }

        float attenuation = emitterCosine * sampleArea
            / max(distanceSquared, 0.01);
        vec3 radiance = emittedRadiance * attenuation * rangeFade;
        result += evaluatePBRDirect(albedo, metallic, roughness, N, V, L, radiance);
    }

    return result;
}
