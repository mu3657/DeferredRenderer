#pragma once

#include "render_pass.h"

#include <vector>

class DDGIVolume;

struct DDGIProbeBlendPassContext : RenderPassFrameContext {
    DDGIVolume& volume;

    DDGIProbeBlendPassContext(
        RenderPassFrameContext& base,
        DDGIVolume& volume_)
        : RenderPassFrameContext(base)
        , volume(volume_)
    {
    }
};

struct DDGIProbeBlendDispatchStats {
    uint32_t firstProbe{0};
    uint32_t probeCount{0};
    uint32_t irradianceTexelCount{0};
    uint32_t distanceTexelCount{0};
};

class DDGIProbeBlendPass : public RenderPassBase {
public:
    const char* name() const override { return "DDGIProbeBlendPass"; }

    void init(const RenderPassInitContext& ctx) override;
    void cleanup() override;
    void execute(DDGIProbeBlendPassContext& ctx);
    void notify_frame_completed(uint32_t frameIndex);
    void draw_debug_ui() override;

    const DDGIProbeBlendDispatchStats& stats() const { return _stats; }
    bool has_history() const { return _hasHistory; }
    bool history_valid(uint64_t serial) const
    {
        return _hasHistory && _lastClearedHistorySerial == serial;
    }
    bool lighting_enabled() const { return _lightingEnabled; }
    float lighting_intensity() const { return _lightingIntensity; }
    int lighting_debug_mode() const { return _lightingDebugMode; }
    float heatmap_exposure() const { return _heatmapExposure; }

private:
    VulkanEngine* _engine{nullptr};
    VkPipelineLayout _pipelineLayout{VK_NULL_HANDLE};
    VkPipeline _pipeline{VK_NULL_HANDLE};
    VkPipelineLayout _rtxgiPipelineLayout{VK_NULL_HANDLE};
    VkPipeline _rtxgiIrradiancePipeline{VK_NULL_HANDLE};
    VkPipeline _rtxgiDistancePipeline{VK_NULL_HANDLE};
    VkPipeline _rtxgiDiagnosticPipeline{VK_NULL_HANDLE};
    std::vector<DDGIProbeBlendDispatchStats> _pendingFrameStats;
    DDGIProbeBlendDispatchStats _stats{};
    uint64_t _lastClearedHistorySerial{0};
    bool _hasHistory{false};
    bool _useOfficialRTXGI{true};
    bool _lightingEnabled{true};
    float _lightingIntensity{1.f};
    float _heatmapExposure{16.f};
    int _lightingDebugMode{0};
};
