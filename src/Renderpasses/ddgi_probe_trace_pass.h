#pragma once

#include "render_pass.h"

#include <vector>

class DDGIVolume;
class RayTracingScene;

struct DDGIProbeTracePassContext : RenderPassFrameContext {
    RayTracingScene& rayTracingScene;
    DDGIVolume& volume;
    LightSystem& lightSystem;

    DDGIProbeTracePassContext(
        RenderPassFrameContext& base,
        RayTracingScene& rayTracingScene_,
        DDGIVolume& volume_,
        LightSystem& lightSystem_)
        : RenderPassFrameContext(base)
        , rayTracingScene(rayTracingScene_)
        , volume(volume_)
        , lightSystem(lightSystem_)
    {
    }
};

struct DDGIProbeTraceDispatchStats {
    uint32_t firstProbe{0};
    uint32_t probeCount{0};
    uint32_t rayCount{0};
    uint32_t traceMode{0};
    uint32_t multiBounce{0};
};

class DDGIProbeTracePass : public RenderPassBase {
public:
    const char* name() const override { return "DDGIProbeTracePass"; }

    void init(const RenderPassInitContext& ctx) override;
    void cleanup() override;
    void execute(DDGIProbeTracePassContext& ctx);
    void notify_frame_completed(uint32_t frameIndex);
    void draw_debug_ui() override;

    const DDGIProbeTraceDispatchStats& stats() const { return _stats; }

private:
    VulkanEngine* _engine{nullptr};
    VkPipelineLayout _pipelineLayout{VK_NULL_HANDLE};
    VkPipeline _pipeline{VK_NULL_HANDLE};
    std::vector<DDGIProbeTraceDispatchStats> _pendingFrameStats;
    DDGIProbeTraceDispatchStats _stats{};
    bool _enabled{true};
    bool _dispatchInFlight{false};
    bool _multiBounceEnabled{true};
    bool _wholeProbeBackfaceRejection{false};
    float _multiBounceStrength{0.85f};
    int _traceMode{3};
};
