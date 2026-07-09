#include "pipeline_registry.h"

#include "material_system.h"
#include "vk_pipelines.h"

#include <stdexcept>

void PipelineRegistry::init(VkDevice device)
{
    _device = device;

    VkPipelineCacheCreateInfo cacheInfo{};
    cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    VK_CHECK(vkCreatePipelineCache(_device, &cacheInfo, nullptr, &_pipelineCache));
}

void PipelineRegistry::cleanup()
{
    if (_device != VK_NULL_HANDLE) {
        for (MaterialPipeline& pipeline : _ownedMaterialPipelines) {
            if (pipeline.pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(_device, pipeline.pipeline, nullptr);
                pipeline.pipeline = VK_NULL_HANDLE;
            }
        }

        for (VkPipelineLayout layout : _ownedPipelineLayouts) {
            if (layout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(_device, layout, nullptr);
            }
        }

        if (_pipelineCache != VK_NULL_HANDLE) {
            vkDestroyPipelineCache(_device, _pipelineCache, nullptr);
            _pipelineCache = VK_NULL_HANDLE;
        }
    }

    _ownedMaterialPipelines.clear();
    _ownedPipelineLayouts.clear();
    _materialPipelines.clear();
    _device = VK_NULL_HANDLE;
}

VkPipelineLayout PipelineRegistry::create_pipeline_layout(const VkPipelineLayoutCreateInfo& createInfo)
{
    if (_device == VK_NULL_HANDLE) {
        throw std::runtime_error("PipelineRegistry must be initialized before creating pipeline layouts");
    }

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VK_CHECK(vkCreatePipelineLayout(_device, &createInfo, nullptr, &layout));
    _ownedPipelineLayouts.push_back(layout);
    return layout;
}

MaterialPipeline* PipelineRegistry::create_material_pipeline(const PipelineKey& key, PipelineBuilder& builder)
{
    auto existing = _materialPipelines.find(key);
    if (existing != _materialPipelines.end()) {
        return existing->second;
    }

    if (_device == VK_NULL_HANDLE) {
        throw std::runtime_error("PipelineRegistry must be initialized before creating pipelines");
    }
    if (builder._pipelineLayout == VK_NULL_HANDLE) {
        throw std::invalid_argument("PipelineRegistry cannot create a pipeline without a pipeline layout");
    }

    VkPipeline pipeline = builder.build_pipeline(_device, _pipelineCache);
    if (pipeline == VK_NULL_HANDLE) {
        throw std::runtime_error("PipelineRegistry failed to create a material pipeline");
    }

    _ownedMaterialPipelines.push_back(MaterialPipeline{
        pipeline,
        builder._pipelineLayout,
    });

    MaterialPipeline* ownedPipeline = &_ownedMaterialPipelines.back();
    _materialPipelines[key] = ownedPipeline;
    return ownedPipeline;
}

MaterialPipeline* PipelineRegistry::get_material_pipeline(const PipelineKey& key) const
{
    auto it = _materialPipelines.find(key);
    if (it == _materialPipelines.end()) {
        return nullptr;
    }

    return it->second;
}

MaterialPipeline* PipelineRegistry::get_material_pipeline(RenderPassType pass, const MaterialInstance& material) const
{
    if (material.technique) {
        if (!material.technique->supports_pass(pass, material.surface)) {
            return nullptr;
        }

        return get_material_pipeline(material.technique->pipeline_key(pass, material.surface));
    }

    PipelineVariant fallbackVariant = material.gbufferVariant;
    if (pass == RenderPassType::ShadowDepth) {
        fallbackVariant = material.surface == MaterialSurface::Masked
            ? PipelineVariant::ShadowDepth_AlphaCutout
            : PipelineVariant::ShadowDepth_Opaque;
    }

    return get_material_pipeline(PipelineKey{
        pass,
        fallbackVariant,
        material.shadingModel,
        material.surface,
    });
}
