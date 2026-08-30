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
    if (!ImGui::Begin("Light System", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

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

    ImGui::End();
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
