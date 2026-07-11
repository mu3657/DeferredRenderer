#pragma once

#include "../GpuData.h"
#include "../render_pass.h"
#include "../vk_types.h"

class LightSystem;
class VulkanEngine;

struct ShadowPassContext : RenderPassFrameContext {
    GPUSceneData& sceneData;
    DrawContext& drawContext;
    LightSystem& lightSystem;

    ShadowPassContext(
        RenderPassFrameContext& base,
        GPUSceneData& sceneData_,
        DrawContext& drawContext_,
        LightSystem& lightSystem_)
        : RenderPassFrameContext(base)
        , sceneData(sceneData_)
        , drawContext(drawContext_)
        , lightSystem(lightSystem_)
    {
    }
};

class ShadowPass : public RenderPassBase {
public:


    struct ShadowFrameResources {
        std::array<AllocatedBuffer, SHADOW_CASCADE_COUNT> sceneBuffers{};
        std::array<VkDescriptorSet, SHADOW_CASCADE_COUNT> sceneDescriptors{};
        AllocatedBuffer shadowDataBuffer;
        VkDescriptorSet descriptor{VK_NULL_HANDLE};
    };

    AllocatedImage _shadowDepthImage{};
    VkSampler _shadowSampler{VK_NULL_HANDLE};
    VkExtent2D _shadowExtent{4096, 4096};

    bool _enabled{true};
    float _maxDistance{50.f};
    float _splitLambda{0.75f};
    float _cascadeBlendRatio{0.1f};
    float _depthPadding{30.f};
    float _bias{0.006f};
    float _strength{0.75f};
    std::array<int, SHADOW_CASCADE_COUNT> _pcfKernelRadii{2, 1, 1, 0};
    std::array<float, SHADOW_CASCADE_COUNT> _lastTexelWorldSize{};
    std::array<float, SHADOW_CASCADE_COUNT> _lastCascadeSplits{};
    std::array<float, SHADOW_CASCADE_COUNT> _lastCascadeBlendWidths{};
    std::array<uint32_t, SHADOW_CASCADE_COUNT> _lastVisibleCasters{};

    MaterialPipeline* _opaquePipeline{nullptr};
    MaterialPipeline* _maskedPipeline{nullptr};

    std::vector<ShadowFrameResources> _frames;
    GPUShadowData _shadowData{};


    const char* name() const override { return "ShadowPass"; }

    void cleanup() override;
    void execute(ShadowPassContext& ctx);

    void update(const LightSystem&, const GPUSceneData&) {}
    void render(VkCommandBuffer, const DrawContext&) {}

    VkDescriptorSet descriptor_set(uint32_t frameIndex) const
    {
        return frameIndex < _frames.size() ? _frames[frameIndex].descriptor : VK_NULL_HANDLE;
    }
    const GPUShadowData& shadow_data() const { return _shadowData; }

    void draw_debug_ui() override;
    void init(const RenderPassInitContext& ctx) override;

private:
    VulkanEngine* _engine{nullptr};

};
