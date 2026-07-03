#pragma once

#include <memory>
#include <vector>

#include "GpuData.h"
#include "vk_types.h"

class VulkanEngine;
struct LoadedScene;

class LightSystem {
public:
    void init(VulkanEngine* engine, uint32_t frameOverlap);
    void cleanup();

    void collect(const LoadedScene* scene, const GPUSceneData& sceneData);
    void upload_frame(uint32_t frameIndex);
    void draw_debug_ui();

    VkDescriptorSet descriptor_set(uint32_t frameIndex) const;
    const GPULightData& light_data() const { return _lightData; }
    const std::vector<GPULight>& lights() const { return _cpuLights; }

private:
    struct FrameResources {
        AllocatedBuffer lightDataBuffer;
        AllocatedBuffer lightBuffer;
        VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
    };

    VulkanEngine* _engine{nullptr};
    std::vector<FrameResources> _frames;
    std::vector<GPULight> _cpuLights;
    GPULightData _lightData{};
    bool _initialized{false};
    bool _enableFallbackDirectional{true};

    void append_fallback_directional(const GPUSceneData& sceneData);
};
