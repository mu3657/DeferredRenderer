#pragma once

#include <memory>
#include <vector>

#include "GpuData.h"
#include "vk_types.h"

class VulkanEngine;
struct FrameData;
struct LoadedScene;

class LightSystem {
public:
    void init(VulkanEngine* engine);
    void cleanup();

    void collect(const LoadedScene* scene, const GPUSceneData& sceneData);
    void upload_frame(FrameData& frame);
    void draw_debug_ui();

    const GPULightData& light_data() const { return _lightData; }
    const std::vector<GPULight>& lights() const { return _cpuLights; }
    GPULight GetDirectionalLight();
    void set_fallback_directional_enabled(bool enabled) { _enableFallbackDirectional = enabled; }
private:
    struct GIResponseReference {
        GPULight light{};
        glm::vec3 stablePower{};
        uint64_t quietProbesRemaining{0};
        bool largeParameterChange{false};
    };
    VulkanEngine* _engine{nullptr};
    std::vector<GPULight> _cpuLights;
    std::vector<GPULight> _previousGILights;
    std::vector<GIResponseReference> _giMotionReferences;
    GPULightData _lightData{};
    bool _initialized{false};
    bool _enableFallbackDirectional{true};

    void append_fallback_directional(const GPUSceneData& sceneData);
};
