#pragma once

#include <cstdint>

#include "GpuData.h"
#include "vk_types.h"

class DescriptorSystem;
class LightSystem;
class VulkanEngine;
struct DrawContext;
struct FrameData;

struct RenderPassInitContext {
    VulkanEngine& engine;
    DescriptorSystem& descriptors;
    VkDevice device;
    VmaAllocator allocator;
};

struct RenderPassFrameContext {
    VulkanEngine& engine;
    FrameData& frame;
    VkCommandBuffer cmd;
    uint32_t frameIndex;
    VkExtent2D drawExtent;

    RenderPassFrameContext(
        VulkanEngine& engine_,
        FrameData& frame_,
        VkCommandBuffer cmd_,
        uint32_t frameIndex_,
        VkExtent2D drawExtent_)
        : engine(engine_)
        , frame(frame_)
        , cmd(cmd_)
        , frameIndex(frameIndex_)
        , drawExtent(drawExtent_)
    {
    }
};

struct GeometryPassContext : RenderPassFrameContext {
    GPUSceneData& sceneData;
    DrawContext& drawContext;

    GeometryPassContext(RenderPassFrameContext& base, GPUSceneData& sceneData_, DrawContext& drawContext_)
        : RenderPassFrameContext(base)
        , sceneData(sceneData_)
        , drawContext(drawContext_)
    {
    }
};

struct LightingPassContext : RenderPassFrameContext {
    VkImageView targetImageView;
    LightSystem& lightSystem;

    LightingPassContext(RenderPassFrameContext& base, VkImageView targetImageView_, LightSystem& lightSystem_)
        : RenderPassFrameContext(base)
        , targetImageView(targetImageView_)
        , lightSystem(lightSystem_)
    {
    }
};

class RenderPassBase {
public:
    RenderPassBase(const RenderPassBase&) = delete;
    RenderPassBase& operator=(const RenderPassBase&) = delete;
    virtual ~RenderPassBase() = default;

    virtual const char* name() const = 0;
    virtual void init(const RenderPassInitContext& ctx) = 0;
    virtual void cleanup() = 0;
    virtual void draw_debug_ui() {}

protected:
    RenderPassBase() = default;
};
