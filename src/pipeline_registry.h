#pragma once

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "vk_types.h"

class PipelineBuilder;

enum class RenderPassType : uint8_t {
    GBuffer,
    ShadowDepth,
    Lighting,
    ForwardOpaque,
    ForwardTransparent,
    Velocity,
    Outline,
};

struct PipelineKey {
    RenderPassType pass{RenderPassType::GBuffer};
    PipelineVariant variant{PipelineVariant::GBuffer_MetallicRoughness};
    ShadingModel shadingModel{ShadingModel::MetallicRoughness};
    MaterialSurface surface{MaterialSurface::Opaque};

    bool operator==(const PipelineKey& other) const
    {
        return pass == other.pass
            && variant == other.variant
            && shadingModel == other.shadingModel
            && surface == other.surface;
    }
};

struct PipelineKeyHash {
    size_t operator()(const PipelineKey& key) const
    {
        const uint32_t pass = static_cast<uint32_t>(key.pass);
        const uint32_t variant = static_cast<uint32_t>(key.variant);
        const uint32_t shadingModel = static_cast<uint32_t>(key.shadingModel);
        const uint32_t surface = static_cast<uint32_t>(key.surface);
        return pass | (variant << 8) | (shadingModel << 16) | (surface << 24);
    }
};

class PipelineRegistry {
public:
    void init(VkDevice device);
    void cleanup();

    VkPipelineLayout create_pipeline_layout(const VkPipelineLayoutCreateInfo& createInfo);
    MaterialPipeline* create_material_pipeline(const PipelineKey& key, PipelineBuilder& builder);

    MaterialPipeline* get_material_pipeline(const PipelineKey& key) const;
    MaterialPipeline* get_material_pipeline(RenderPassType pass, const MaterialInstance& material) const;

private:
    VkDevice _device{VK_NULL_HANDLE};
    VkPipelineCache _pipelineCache{VK_NULL_HANDLE};
    std::vector<VkPipelineLayout> _ownedPipelineLayouts;
    std::deque<MaterialPipeline> _ownedMaterialPipelines;
    std::unordered_map<PipelineKey, MaterialPipeline*, PipelineKeyHash> _materialPipelines;
};
