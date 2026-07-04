#pragma once

#include <cstdint>
#include <unordered_map>

#include "vk_types.h"

class VulkanEngine;

enum class RenderPassType : uint8_t {
    GBuffer,
    ShadowDepth,
    Lighting,
};

struct PipelineKey {
    RenderPassType pass{RenderPassType::GBuffer};
    PipelineVariant variant{PipelineVariant::GBuffer_MetallicRoughness};
    MaterialSurface materialPass{MaterialSurface::MainColor};

    bool operator==(const PipelineKey& other) const
    {
        return pass == other.pass && variant == other.variant && materialPass == other.materialPass;
    }
};

struct PipelineKeyHash {
    size_t operator()(const PipelineKey& key) const
    {
        const uint32_t pass = static_cast<uint32_t>(key.pass);
        const uint32_t variant = static_cast<uint32_t>(key.variant);
        const uint32_t materialPass = static_cast<uint32_t>(key.materialPass);
        return pass | (variant << 8) | (materialPass << 16);
    }
};

class PipelineRegistry {
public:
    void init(VulkanEngine* engine);
    void cleanup();

    void register_material_pipeline(const PipelineKey& key, MaterialPipeline* pipeline);
    MaterialPipeline* get_material_pipeline(const PipelineKey& key) const;

private:
    VulkanEngine* _engine{nullptr};
    std::unordered_map<PipelineKey, MaterialPipeline*, PipelineKeyHash> _materialPipelines;
};
