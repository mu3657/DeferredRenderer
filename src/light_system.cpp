#include "light_system.h"

#include "imgui.h"
#include "vk_engine.h"
#include "vk_loader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtx/norm.hpp>

namespace {
glm::vec3 safe_normalize(glm::vec3 v, glm::vec3 fallback)
{
    if (glm::length2(v) < 0.000001f) {
        return fallback;
    }

    return glm::normalize(v);
}

glm::vec3 light_forward_from_world(const glm::mat4& worldTransform)
{
    return safe_normalize(glm::mat3(worldTransform) * glm::vec3(0.f, 0.f, -1.f), glm::vec3(0.f, -1.f, 0.f));
}

glm::vec3 light_right_from_world(const glm::mat4& worldTransform, const glm::vec3& forward)
{
    glm::vec3 right = glm::mat3(worldTransform) * glm::vec3(1.f, 0.f, 0.f);
    right -= forward * glm::dot(right, forward);
    return safe_normalize(right, glm::vec3(1.f, 0.f, 0.f));
}

glm::mat4 compute_world_transform(const Node& node)
{
    if (std::shared_ptr<Node> parent = node.parent.lock()) {
        return compute_world_transform(*parent) * node.localTransform;
    }

    return node.localTransform;
}

bool is_visible_in_hierarchy(const Node& node)
{
    if (!node.visible) {
        return false;
    }

    if (std::shared_ptr<Node> parent = node.parent.lock()) {
        return is_visible_in_hierarchy(*parent);
    }

    return true;
}

uint32_t light_type_to_uint(LightType type)
{
    return static_cast<uint32_t>(type);
}

glm::vec3 light_energy(const GPULight& light)
{
    if (light.directionType.w != static_cast<float>(LightType::Directional)
        && light.positionRange.w <= 0.f) return glm::vec3(0.f);
    return glm::max(glm::vec3(light.colorIntensity), glm::vec3(0.f))
        * std::max(light.colorIntensity.w, 0.f);
}

float max_component(glm::vec3 value)
{
    return std::max({value.x, value.y, value.z});
}

glm::vec3 light_response_power(const GPULight& light)
{
    glm::vec3 power = light_energy(light);
    const auto type = static_cast<LightType>(static_cast<int>(light.directionType.w));
    if (type != LightType::Directional) power *= std::max(light.positionRange.w, 0.f);
    if (type == LightType::Spot) power *= std::max(0.f, 1.f - .5f * (light.params.x + light.params.y));
    if (type == LightType::RectArea) power *= std::max(.001f, 4.f * light.areaRight.w * light.areaUp.w);
    return power;
}

bool requires_gi_radiance_refresh(const GPULight& previous, const GPULight& current)
{
    // Continuous source parameters, including zero crossings, must retain the
    // other lights' history and multi-bounce energy. Only discrete changes use
    // the hard-refresh path; raster shadow-slot indices are bookkeeping.
    return previous.directionType.w != current.directionType.w || previous.params.w != current.params.w;
}

bool light_parameters_exceed_threshold(const GPULight& reference, const GPULight& current)
{
    const glm::vec3 oldEnergy = light_energy(reference), newEnergy = light_energy(current);
    const float energyScale = std::max(max_component(oldEnergy), max_component(newEnergy));
    if (max_component(glm::abs(newEnergy - oldEnergy)) > std::max(.001f, .02f * energyScale)) return true;
    if (energyScale <= 0.f) return false;
    const auto changed = [](float a, float b, float floor) {
        return std::abs(a - b) > std::max(floor, .02f * std::max(std::abs(a), std::abs(b)));
    };
    const auto type = static_cast<LightType>(static_cast<int>(current.directionType.w));
    if (type != LightType::Directional && changed(reference.positionRange.w, current.positionRange.w, .01f)) return true;
    if (type == LightType::Spot
        && (std::abs(reference.params.x - current.params.x) > .002f
            || std::abs(reference.params.y - current.params.y) > .002f)) return true;
    return type == LightType::RectArea
        && (changed(reference.areaRight.w, current.areaRight.w, .001f)
            || changed(reference.areaUp.w, current.areaUp.w, .001f));
}

bool light_motion_exceeds_threshold(const GPULight& reference, const GPULight& current, float positionThreshold)
{
    // Moving or rotating an extinguished emitter must not disturb other lights.
    if (max_component(light_energy(current)) <= 0.f) return false;
    const auto type = static_cast<LightType>(static_cast<int>(current.directionType.w));
    constexpr float responseAngle = 0.5f * 3.14159265359f / 180.f;
    const float angleCosine = std::cos(responseAngle);
    if (type != LightType::Directional && glm::distance2(glm::vec3(reference.positionRange),
            glm::vec3(current.positionRange)) > positionThreshold * positionThreshold) return true;
    if ((type == LightType::Directional || type == LightType::Spot)
        && glm::dot(glm::vec3(reference.directionType), glm::vec3(current.directionType)) < angleCosine) return true;
    // Point-light orientation has no effect. Rectangular emitters instead use
    // their sampling basis, including roll around their normal.
    return type == LightType::RectArea
        && (glm::dot(glm::vec3(reference.areaRight), glm::vec3(current.areaRight)) < angleCosine
            || glm::dot(glm::vec3(reference.areaUp), glm::vec3(current.areaUp)) < angleCosine);
}
}

void LightSystem::init(VulkanEngine* engine)
{
    _engine = engine;
    _cpuLights.reserve(MAX_GPU_LIGHTS);
    _initialized = true;
}

void LightSystem::cleanup()
{
    if (!_initialized || !_engine) {
        return;
    }

    _cpuLights.clear();
    _previousGILights.clear();
    _giMotionReferences.clear();
    _engine = nullptr;
    _initialized = false;
}

void LightSystem::collect(const LoadedScene* scene, const GPUSceneData& sceneData)
{
    _cpuLights.clear();
    _lightData = {};
    _lightData.ambientColor = sceneData.ambientColor;

    uint32_t punctualShadowCount = 0;
    uint32_t punctualShadowTileCount = 0;

    if (scene) {
        for (const std::weak_ptr<LightNode>& weakLight : scene->lightNodes) {
            if (_cpuLights.size() >= MAX_GPU_LIGHTS) {
                break;
            }

            std::shared_ptr<LightNode> lightNode = weakLight.lock();
            if (!lightNode || !is_visible_in_hierarchy(*lightNode)) {
                continue;
            }

            GPULight gpuLight{};
            const GpuLight& src = lightNode->light;
            const glm::mat4 worldTransform = compute_world_transform(*lightNode);
            const glm::vec3 worldPosition = glm::vec3(worldTransform[3]);
            const glm::vec3 forward = light_forward_from_world(worldTransform);
            const glm::vec3 right = light_right_from_world(worldTransform, forward);
            const glm::vec3 up = safe_normalize(glm::cross(right, forward), glm::vec3(0.f, 1.f, 0.f));
            const float range = (src.type == LightType::Directional) ? 0.f : std::max(src.range, 0.f);

            gpuLight.positionRange = glm::vec4(worldPosition, range);
            gpuLight.directionType = glm::vec4(forward, static_cast<float>(light_type_to_uint(src.type)));
            gpuLight.colorIntensity = glm::vec4(src.color, src.intensity);
            const float outerHalfAngle = glm::radians(glm::clamp(src.spotSizeDegrees, 1.f, 179.f) * 0.5f);
            const float innerHalfAngle = outerHalfAngle * (1.f - glm::clamp(src.spotBlend, 0.f, 1.f));
            gpuLight.params = glm::vec4(
                std::cos(innerHalfAngle),
                std::cos(outerHalfAngle),
                -1.f,
                src.castsShadow ? 1.f : 0.f);

            const bool punctual = src.type == LightType::Point || src.type == LightType::Spot;
            const uint32_t requiredShadowTiles = src.type == LightType::Point ? 6u : 1u;
            if (punctual
                && src.castsShadow
                && src.intensity > 0.f
                && range > 0.f
                && punctualShadowCount < MAX_PUNCTUAL_SHADOWS
                && punctualShadowTileCount + requiredShadowTiles <= MAX_PUNCTUAL_SHADOW_TILES) {
                gpuLight.params.z = static_cast<float>(punctualShadowCount++);
                punctualShadowTileCount += requiredShadowTiles;
            }
            gpuLight.areaRight = glm::vec4(right, std::max(src.width * 0.5f, 0.005f));
            gpuLight.areaUp = glm::vec4(up, std::max(src.height * 0.5f, 0.005f));

            switch (src.type) {
                case LightType::Directional:
                    _lightData.directionalLightCount++;
                    break;
                case LightType::Point:
                    _lightData.pointLightCount++;
                    break;
                case LightType::Spot:
                    _lightData.spotLightCount++;
                    break;
                case LightType::RectArea:
                    _lightData.rectAreaLightCount++;
                    break;
            }

            _cpuLights.push_back(gpuLight);
        }
    }

    if (_lightData.directionalLightCount == 0
        && _enableFallbackDirectional
        && _cpuLights.size() < MAX_GPU_LIGHTS) {
        append_fallback_directional(sceneData);
    }

    _lightData.lightCount = static_cast<uint32_t>(_cpuLights.size());
    const bool allSourcesExtinguished = std::any_of(_previousGILights.begin(), _previousGILights.end(),
            [](const GPULight& light) { return max_component(light_energy(light)) > 0.f; })
        && std::none_of(_cpuLights.begin(), _cpuLights.end(),
            [](const GPULight& light) { return max_component(light_energy(light)) > 0.f; })
        && _engine && !_engine->pbrEmissiveEnabled
        && (!_engine->environmentLighting.ddgiLightingEnabled || _engine->environmentLighting.intensity <= 0.f);
    // With no remaining source, a hard refresh guarantees black without
    // feeding the last source's old bounce energy back into the volume.
    const bool lightingChanged = allSourcesExtinguished || _previousGILights.size() != _cpuLights.size()
        || !std::equal(_previousGILights.begin(), _previousGILights.end(), _cpuLights.begin(),
            [](const GPULight& previous, const GPULight& current) {
                return !requires_gi_radiance_refresh(previous, current);
            });
    if (lightingChanged && _engine && _engine->ddgiVolume.initialized()) {
        _engine->ddgiVolume.notify_lighting_changed();
    }
    if (lightingChanged || _giMotionReferences.size() != _cpuLights.size()) {
        _giMotionReferences.clear();
        for (const auto& light : _cpuLights)
            _giMotionReferences.push_back({light, light_response_power(light), 0u, false});
    } else {
        // Anchors let slow translations/rotations accumulate without treating
        // bounded sub-threshold jitter as a sustained light change.
        const glm::vec3 spacing = _engine && _engine->ddgiVolume.initialized()
            ? _engine->ddgiVolume.desc().probeSpacing : glm::vec3(1.f);
        const float positionThreshold = glm::clamp(std::min({spacing.x, spacing.y, spacing.z}) * .01f, .005f, .05f);
        const bool volumeReady = _engine && _engine->ddgiVolume.initialized();
        const uint32_t updated = volumeReady ? _engine->ddgiVolume.update_range().probeCount : 0;
        const uint64_t quietWindow = volumeReady ? uint64_t(_engine->ddgiVolume.total_probe_count()) * 64 : 1;
        const bool responseActive = volumeReady && _engine->ddgiVolume.lighting_parameter_response_active();
        bool parameterEvent = false, largeStateChanged = false, anyLargeChange = false;
        for (size_t i = 0; i < _cpuLights.size(); ++i) {
            auto& state = _giMotionReferences[i];
            auto& reference = state.light;
            const auto& current = _cpuLights[i];
            const bool previousLarge = state.largeParameterChange;
            state.quietProbesRemaining -= std::min<uint64_t>(state.quietProbesRemaining, updated);
            if (state.quietProbesRemaining == 0) {
                state.stablePower = light_response_power(_previousGILights[i]);
                state.largeParameterChange = false;
            }
            const bool motion = light_motion_exceeds_threshold(reference, current, positionThreshold);
            const bool parameters = light_parameters_exceed_threshold(reference, current);
            if (parameters) state.quietProbesRemaining = quietWindow;
            if (state.quietProbesRemaining > 0) {
                const glm::vec3 power = light_response_power(current);
                const float scale = std::max(1.e-6f, std::max(max_component(power), max_component(state.stablePower)));
                const float change = max_component(glm::abs(power - state.stablePower)) / scale;
                if (change >= .25f) state.largeParameterChange = true;
                else if (change <= .1f) state.largeParameterChange = false;
            }
            parameterEvent |= parameters;
            largeStateChanged |= previousLarge != state.largeParameterChange;
            anyLargeChange |= state.largeParameterChange;
            if (motion || parameters) {
                if (_engine && _engine->ddgiVolume.initialized()) {
                    if (motion) _engine->ddgiVolume.notify_lighting_motion(
                        _cpuLights[i].directionType.w != static_cast<float>(LightType::Directional));
                }
                // Keep independent component anchors: moving every frame must
                // not erase accumulated sub-threshold brightness/range edits.
                if (motion) {
                    reference.positionRange = glm::vec4(glm::vec3(current.positionRange), reference.positionRange.w);
                    reference.directionType = current.directionType;
                    reference.areaRight = glm::vec4(glm::vec3(current.areaRight), reference.areaRight.w);
                    reference.areaUp = glm::vec4(glm::vec3(current.areaUp), reference.areaUp.w);
                }
                if (parameters) {
                    reference.colorIntensity = current.colorIntensity;
                    reference.positionRange.w = current.positionRange.w;
                    reference.areaRight.w = current.areaRight.w;
                    reference.areaUp.w = current.areaUp.w;
                    reference.params.x = current.params.x;
                    reference.params.y = current.params.y;
                }
            }
        }
        if (volumeReady && (parameterEvent || (largeStateChanged && responseActive)))
            _engine->ddgiVolume.notify_lighting_parameters(anyLargeChange);
    }
    _previousGILights = _cpuLights;
}

void LightSystem::upload_frame(FrameData& frame)
{
    if (!_initialized) {
        return;
    }

    memcpy(frame.lightDataBuffer.info.pMappedData, &_lightData, sizeof(GPULightData));
    VK_CHECK(vmaFlushAllocation(
        _engine->_allocator,
        frame.lightDataBuffer.allocation,
        0,
        sizeof(GPULightData)));

    if (!_cpuLights.empty()) {
        const size_t copySize = std::min(_cpuLights.size(), static_cast<size_t>(MAX_GPU_LIGHTS)) * sizeof(GPULight);
        memcpy(frame.lightBuffer.info.pMappedData, _cpuLights.data(), copySize);
        VK_CHECK(vmaFlushAllocation(
            _engine->_allocator,
            frame.lightBuffer.allocation,
            0,
            copySize));
    }
}

void LightSystem::draw_debug_ui()
{
    ImGui::Checkbox("Fallback directional", &_enableFallbackDirectional);
    ImGui::Text("Lights: %u / %u", _lightData.lightCount, MAX_GPU_LIGHTS);
    ImGui::Text("Directional: %u", _lightData.directionalLightCount);
    ImGui::Text("Point: %u", _lightData.pointLightCount);
    ImGui::Text("Spot: %u", _lightData.spotLightCount);
    ImGui::Text("Rect Area: %u", _lightData.rectAreaLightCount);

    ImGui::Separator();
    for (uint32_t i = 0; i < _lightData.lightCount && i < static_cast<uint32_t>(_cpuLights.size()); i++) {
        const GPULight& light = _cpuLights[i];
        const int type = static_cast<int>(light.directionType.w);
        const char* typeName = "Unknown";
        if (type == static_cast<int>(LightType::Directional)) {
            typeName = "Directional";
        } else if (type == static_cast<int>(LightType::Point)) {
            typeName = "Point";
        } else if (type == static_cast<int>(LightType::Spot)) {
            typeName = "Spot";
        } else if (type == static_cast<int>(LightType::RectArea)) {
            typeName = "Rect Area";
        }

        ImGui::Text("#%u %s intensity %.2f range %.2f", i, typeName, light.colorIntensity.w, light.positionRange.w);
    }

}

 GPULight LightSystem::GetDirectionalLight()
{
    for (GPULight DL:_cpuLights)
    {
        if (static_cast<int>(DL.directionType.w) == static_cast<int>(LightType::Directional))
        {
            return DL;
        }
    }
    GPULight defaultDirectionalLight = {};
    defaultDirectionalLight.positionRange = glm::vec4(0.f, 0.f, 0.f, 0.f);
    defaultDirectionalLight.directionType = glm::vec4(0.f, -1.f, 0.f, static_cast<float>(light_type_to_uint(LightType::Directional)));
    defaultDirectionalLight.colorIntensity = glm::vec4(1.f, 1.f, 1.f, 1.f);
    defaultDirectionalLight.params = glm::vec4(1.f, 1.f, -1.f, 1.f);
    defaultDirectionalLight.areaRight = glm::vec4(0.f);
    defaultDirectionalLight.areaUp = glm::vec4(0.f);

    return  defaultDirectionalLight;
}

void LightSystem::append_fallback_directional(const GPUSceneData& sceneData)
{
    const glm::vec3 lightToSurface = safe_normalize(-glm::vec3(sceneData.sunlightDirection), glm::vec3(0.f, -1.f, -0.5f));

    GPULight fallback{};
    fallback.positionRange = glm::vec4(0.f, 0.f, 0.f, 0.f);
    fallback.directionType = glm::vec4(lightToSurface, static_cast<float>(light_type_to_uint(LightType::Directional)));
    fallback.colorIntensity = sceneData.sunlightColor;
    fallback.params = glm::vec4(1.f, 1.f, -1.f, 1.f);
    fallback.areaRight = glm::vec4(0.f);
    fallback.areaUp = glm::vec4(0.f);

    _cpuLights.push_back(fallback);
    _lightData.directionalLightCount = 1;
}
