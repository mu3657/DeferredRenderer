#include "material_system.h"

bool MetallicRoughnessTechnique::supports_pass(RenderPassType pass, MaterialSurface surface) const
{
    switch (pass) {
    case RenderPassType::GBuffer:
        return surface == MaterialSurface::Opaque
            || surface == MaterialSurface::Masked
            || surface == MaterialSurface::Transparent;
    case RenderPassType::ShadowDepth:
        return surface == MaterialSurface::Opaque || surface == MaterialSurface::Masked;
    case RenderPassType::Lighting:
    case RenderPassType::ForwardOpaque:
    case RenderPassType::ForwardTransparent:
    case RenderPassType::Velocity:
    case RenderPassType::Outline:
        return false;
    }

    return false;
}

PipelineKey MetallicRoughnessTechnique::pipeline_key(RenderPassType pass, MaterialSurface surface) const
{
    PipelineVariant variant = PipelineVariant::GBuffer_MetallicRoughness;
    if (pass == RenderPassType::ShadowDepth) {
        variant = surface == MaterialSurface::Masked
            ? PipelineVariant::ShadowDepth_AlphaCutout
            : PipelineVariant::ShadowDepth_Opaque;
    }

    return PipelineKey{
        pass,
        variant,
        ShadingModel::MetallicRoughness,
        surface,
    };
}
