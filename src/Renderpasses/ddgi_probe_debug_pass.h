#pragma once

#include "render_pass.h"

class DDGIVolume;

struct DDGIProbeDebugPassContext : RenderPassFrameContext {
    DDGIVolume& volume;
    VkImageView targetImageView;
    VkImageView depthImageView;

    DDGIProbeDebugPassContext(
        RenderPassFrameContext& base,
        DDGIVolume& volume_,
        VkImageView targetImageView_,
        VkImageView depthImageView_)
        : RenderPassFrameContext(base)
        , volume(volume_)
        , targetImageView(targetImageView_)
        , depthImageView(depthImageView_)
    {
    }
};

class DDGIProbeDebugPass : public RenderPassBase {
public:
    const char* name() const override { return "DDGIProbeDebugPass"; }

    void init(const RenderPassInitContext& ctx) override;
    void cleanup() override;
    void execute(DDGIProbeDebugPassContext& ctx);
    void draw_debug_ui() override;
    bool fit_volume_to_active_scene(DDGIVolume& volume);

private:
    VulkanEngine* _engine{nullptr};
    VkPipelineLayout _pipelineLayout{VK_NULL_HANDLE};
    VkPipeline _pipeline{VK_NULL_HANDLE};
    VkPipeline _xrayPipeline{VK_NULL_HANDLE};
    bool _enabled{true};
    bool _xray{true};
    bool _screenSpaceTest{false};
    bool _cameraFrontTest{false};
    bool _currentBatchOnly{false};
    float _radius{0.12f};
    float _irradianceIntensity{1.f};
    int _mode{0};
    uint32_t _lastDrawnProbeCount{0};
};
