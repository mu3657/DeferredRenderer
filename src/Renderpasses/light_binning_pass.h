#pragma once

#include "render_pass.h"

class LightBinningPass : public RenderPassBase {
public:
    static constexpr uint32_t TileSize = 16;
    static constexpr uint32_t LightMaskWordCount = (MAX_GPU_LIGHTS + 31u) / 32u;

    const char* name() const override { return "LightBinningPass"; }

    void init(const RenderPassInitContext& ctx) override;
    void cleanup() override;
    void execute(RenderPassFrameContext& ctx);
    void draw_debug_ui() override;

    bool enabled() const { return _enabled; }
    bool debug_heatmap() const { return _debugHeatmap; }
    uint32_t tile_count_x(VkExtent2D extent) const;
    uint32_t tile_count_y(VkExtent2D extent) const;

private:
    VulkanEngine* _engine{nullptr};
    VkPipelineLayout _pipelineLayout{VK_NULL_HANDLE};
    VkPipeline _pipeline{VK_NULL_HANDLE};
    bool _enabled{true};
    bool _debugHeatmap{false};
    uint32_t _lastTileCountX{0};
    uint32_t _lastTileCountY{0};
    VkDeviceSize _maskBufferSize{0};
};
