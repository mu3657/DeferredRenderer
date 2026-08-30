#pragma once

#include "render_pass.h"

class ToneMapPass : public RenderPassBase {
public:
    const char* name() const override { return "ToneMapPass"; }

    void init(const RenderPassInitContext& ctx) override;
    void cleanup() override;
    void execute(RenderPassFrameContext& ctx);
    void draw_debug_ui() override;

private:
    VulkanEngine* _engine{nullptr};
    VkPipelineLayout _pipelineLayout{VK_NULL_HANDLE};
    VkPipeline _pipeline{VK_NULL_HANDLE};
    bool _enabled{true};
    float _exposure{1.f};
};
