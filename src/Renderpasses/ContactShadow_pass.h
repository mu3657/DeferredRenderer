#pragma once

#include "render_pass.h"

struct ContactShadowPassContext : RenderPassFrameContext {
    GPUSceneData& sceneData;
    LightSystem& lightSystem;

    ContactShadowPassContext(
        RenderPassFrameContext& base,
        GPUSceneData& sceneData_,
        LightSystem& lightSystem_)
        : RenderPassFrameContext(base)
        , sceneData(sceneData_)
        , lightSystem(lightSystem_)
    {
    }
};

class ContactShadowPass : public RenderPassBase {
public:
    const char* name() const override { return "ContactShadowPass"; }

    void init(const RenderPassInitContext& ctx) override;
    void cleanup() override;
    void execute(ContactShadowPassContext& ctx);
    void draw_debug_ui() override;

    VkDescriptorSet lighting_descriptor_set() const { return _lightingDescriptorSet; }

private:
    VulkanEngine* _engine{nullptr};
    VkPipelineLayout _pipelineLayout{VK_NULL_HANDLE};
    VkPipeline _pipeline{VK_NULL_HANDLE};
    VkDescriptorSet _computeDescriptorSet{VK_NULL_HANDLE};
    VkDescriptorSet _lightingDescriptorSet{VK_NULL_HANDLE};

    bool _enabled{true};
    int _stepCount{24};
    float _maxDistance{1.5f};
    float _thickness{0.08f};
    float _normalBias{0.02f};
    float _strength{1.0f};
    float _edgeFade{0.05f};
    float _stepExponent{1.0f};
};
