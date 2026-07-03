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

// Basic PBR Lighting functions (simplified for minimalism)
const float PI = 3.14159265359;

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

    // Discard background (skybox logic can run before/after this)
    if (depth >= 1.0) {
        outFragColor = vec4(0.0); // Or sample a skybox here if you want
        return;
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

    // --- Direct Lighting ---
    for (uint i = 0; i < lightData.lightCount; i++) {
        GPULight light = lights[i];
        uint type = uint(light.directionType.w + 0.5);

        vec3 L = vec3(0.0);
        vec3 radiance = light.colorIntensity.rgb * light.colorIntensity.w;

        if (type == LIGHT_TYPE_DIRECTIONAL) {
            // directionType.xyz is the light emission direction. Shading uses the surface-to-light vector.
            L = normalize(-light.directionType.xyz);
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

        Lo += evaluatePBRDirect(albedo, metallic, roughness, N, V, L, radiance);
    }

    // --- Ambient Lighting ---
    vec3 ambient = lightData.ambientColor.rgb * albedo * ao;

    vec3 color = ambient + Lo;

    // --- HDR & Gamma Correction (if not using sRGB output target) ---
    // color = color / (color + vec3(1.0)); // simple tone mapping
    // color = pow(color, vec3(1.0/2.2)); 

    outFragColor = vec4(color, 1.0);
}
