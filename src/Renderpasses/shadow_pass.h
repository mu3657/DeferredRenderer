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
        AllocatedBuffer punctualSceneBuffer;
        std::array<VkDescriptorSet, MAX_PUNCTUAL_SHADOW_TILES> punctualSceneDescriptors{};
        AllocatedBuffer shadowDataBuffer;
        AllocatedBuffer punctualShadowDataBuffer;
        VkDescriptorSet descriptor{VK_NULL_HANDLE};
    };

    AllocatedImage _shadowDepthImage{};
    AllocatedImage _punctualShadowDepthImage{};
    VkSampler _shadowSampler{VK_NULL_HANDLE};
    VkExtent2D _shadowExtent{4096, 4096};
    VkExtent2D _punctualShadowExtent{4096, 4096};
    uint32_t _punctualTileResolution{512};
    size_t _punctualSceneStride{0};

    bool _enabled{true};
    float _maxDistance{50.f};
    float _splitLambda{0.75f};
    float _cascadeBlendRatio{0.1f};
    float _depthPadding{30.f};
    float _receiverBiasTexels{0.1f};
    float _rasterConstantBias{1.25f};
    float _rasterSlopeBias{1.75f};
    float _strength{1.0f};
    bool _punctualEnabled{true};
    float _punctualNormalBias{0.02f};
    float _punctualRasterConstantBias{1.25f};
    float _punctualRasterSlopeBias{1.75f};
    float _punctualStrength{1.f};
    std::array<int, SHADOW_CASCADE_COUNT> _pcfKernelRadii{2, 1, 1, 0};
    std::array<float, SHADOW_CASCADE_COUNT> _lastTexelWorldSize{};
    std::array<float, SHADOW_CASCADE_COUNT> _lastDepthRange{};
    std::array<float, SHADOW_CASCADE_COUNT> _lastCascadeSplits{};
    std::array<float, SHADOW_CASCADE_COUNT> _lastCascadeBlendWidths{};
    std::array<uint32_t, SHADOW_CASCADE_COUNT> _lastVisibleCasters{};

    MaterialPipeline* _opaquePipeline{nullptr};
    MaterialPipeline* _maskedPipeline{nullptr};

    std::vector<ShadowFrameResources> _frames;
    GPUShadowData _shadowData{};
    GPUPunctualShadowData _punctualShadowData{};
    uint32_t _lastPunctualShadowCount{0};
    uint32_t _lastPunctualFaceCount{0};
    uint32_t _lastPunctualCasterDraws{0};


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
