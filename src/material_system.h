#pragma once

#include "pipeline_registry.h"

class MaterialTechnique {
public:
    virtual ~MaterialTechnique() = default;

    virtual ShadingModel shading_model() const = 0;
    virtual bool supports_pass(RenderPassType pass, MaterialSurface surface) const = 0;
    virtual PipelineKey pipeline_key(RenderPassType pass, MaterialSurface surface) const = 0;
};

class MetallicRoughnessTechnique final : public MaterialTechnique {
public:
    ShadingModel shading_model() const override { return ShadingModel::MetallicRoughness; }
    bool supports_pass(RenderPassType pass, MaterialSurface surface) const override;
    PipelineKey pipeline_key(RenderPassType pass, MaterialSurface surface) const override;
};
