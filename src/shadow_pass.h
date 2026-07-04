#pragma once

#include "GpuData.h"
#include "render_pass.h"
#include "vk_types.h"

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
    const char* name() const override { return "ShadowPass"; }

    void init(const RenderPassInitContext& ctx) override { _engine = &ctx.engine; }
    void cleanup() override { _engine = nullptr; }
    void execute(ShadowPassContext& ctx)
    {
        update(ctx.lightSystem, ctx.sceneData);
        render(ctx.cmd, ctx.drawContext);
    }

    void update(const LightSystem&, const GPUSceneData&) {}
    void render(VkCommandBuffer, const DrawContext&) {}

    VkDescriptorSet descriptor_set() const { return VK_NULL_HANDLE; }
    const GPUShadowData& shadow_data() const { return _shadowData; }

    void draw_debug_ui() override {}

private:
    VulkanEngine* _engine{nullptr};
    GPUShadowData _shadowData{};
};
