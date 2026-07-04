#include "pipeline_registry.h"

#include <stdexcept>

void PipelineRegistry::init(VulkanEngine* engine)
{
    _engine = engine;
}

void PipelineRegistry::cleanup()
{
    _materialPipelines.clear();
    _engine = nullptr;
}

void PipelineRegistry::register_material_pipeline(const PipelineKey& key, MaterialPipeline* pipeline)
{
    if (!pipeline) {
        throw std::invalid_argument("PipelineRegistry cannot register a null material pipeline");
    }

    _materialPipelines[key] = pipeline;
}

MaterialPipeline* PipelineRegistry::get_material_pipeline(const PipelineKey& key) const
{
    auto it = _materialPipelines.find(key);
    if (it == _materialPipelines.end()) {
        return nullptr;
    }

    return it->second;
}
