// Shared metallic-roughness BRDF used by deferred and forward lighting.
// Inputs are linear-light values. Roughness is perceptual roughness, matching glTF.
const float PI = 3.14159265358979323846;

vec3 pbrF0(vec3 baseColor, float metallic)
{
    return mix(vec3(0.04), baseColor, clamp(metallic, 0.0, 1.0));
}

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    float factor = pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
    return F0 + (vec3(1.0) - F0) * factor;
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
    vec3 grazing = max(vec3(1.0 - roughness), F0);
    float factor = pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
    return F0 + (grazing - F0) * factor;
}

float distributionGGX(float NdotH, float roughness)
{
    float alpha = max(roughness * roughness, 0.0016);
    float alphaSquared = alpha * alpha;
    float denominator = NdotH * NdotH * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / max(PI * denominator * denominator, 1e-6);
}

// Height-correlated Smith visibility term. This directly returns G / (4 NoV NoL),
// which is more stable at grazing angles than dividing the three BRDF terms later.
float visibilitySmithGGXCorrelated(float NdotV, float NdotL, float roughness)
{
    float alpha = max(roughness * roughness, 0.0016);
    float alphaSquared = alpha * alpha;
    float lambdaV = NdotL * sqrt(max(NdotV * NdotV * (1.0 - alphaSquared) + alphaSquared, 0.0));
    float lambdaL = NdotV * sqrt(max(NdotL * NdotL * (1.0 - alphaSquared) + alphaSquared, 0.0));
    return 0.5 / max(lambdaV + lambdaL, 1e-6);
}

vec3 pbrDiffuseEnergy(vec3 baseColor, float metallic, float roughness, float NdotV)
{
    vec3 F = fresnelSchlickRoughness(
        clamp(NdotV, 0.0, 1.0),
        pbrF0(baseColor, metallic),
        clamp(roughness, 0.04, 1.0));
    return (vec3(1.0) - F) * (1.0 - clamp(metallic, 0.0, 1.0));
}

vec3 evaluatePBRDiffuseIrradiance(
    vec3 irradiance,
    vec3 baseColor,
    float metallic,
    float roughness,
    float NdotV)
{
    return max(irradiance, vec3(0.0))
        * pbrDiffuseEnergy(baseColor, metallic, roughness, NdotV)
        * baseColor
        / PI;
}

vec3 evaluatePBRDirect(
    vec3 baseColor,
    float metallic,
    float roughness,
    vec3 N,
    vec3 V,
    vec3 L,
    vec3 radiance)
{
    float NdotV = clamp(dot(N, V), 0.0, 1.0);
    float NdotL = clamp(dot(N, L), 0.0, 1.0);
    if (NdotV <= 0.0 || NdotL <= 0.0) {
        return vec3(0.0);
    }

    vec3 halfVector = V + L;
    float halfLengthSquared = dot(halfVector, halfVector);
    if (halfLengthSquared <= 1e-8) {
        return vec3(0.0);
    }

    vec3 H = halfVector * inversesqrt(halfLengthSquared);
    float clampedRoughness = clamp(roughness, 0.04, 1.0);
    vec3 F = fresnelSchlick(clamp(dot(H, V), 0.0, 1.0), pbrF0(baseColor, metallic));
    float D = distributionGGX(clamp(dot(N, H), 0.0, 1.0), clampedRoughness);
    float visibility = visibilitySmithGGXCorrelated(NdotV, NdotL, clampedRoughness);

    vec3 specular = D * visibility * F;
    vec3 diffuse = (vec3(1.0) - F)
        * (1.0 - clamp(metallic, 0.0, 1.0))
        * baseColor
        / PI;
    return (diffuse + specular) * max(radiance, vec3(0.0)) * NdotL;
}
