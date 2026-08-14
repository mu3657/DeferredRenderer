// Copyright 2023 Sony Interactive Entertainment.
// Licensed under the Apache License, Version 2.0.
// Vulkan/SPIR-V integration wrapper for Bend Studio Screen Space Shadows.

#define WAVE_SIZE 64
#define SAMPLE_COUNT 60
#define HARD_SHADOW_SAMPLES 4
#define FADE_OUT_SAMPLES 8

struct BendPushConstants
{
    float4 LightCoordinate;
    int4 WaveOffset;
    float4 Tuning;       // surface thickness, bilinear threshold, contrast, strength
    float4 DepthParams;  // inv width, inv height, far depth, near depth
    float4 DepthBounds;  // min depth, max depth, unused, unused
    uint4 Flags;         // packed feature bits in x
};

[[vk::push_constant]] ConstantBuffer<BendPushConstants> PushConstants;

static const uint FLAG_IGNORE_EDGE_PIXELS          = 1u << 0;
static const uint FLAG_USE_PRECISION_OFFSET        = 1u << 1;
static const uint FLAG_BILINEAR_SAMPLING_OFFSET    = 1u << 2;
static const uint FLAG_DEBUG_EDGE_MASK             = 1u << 3;
static const uint FLAG_DEBUG_THREAD_INDEX          = 1u << 4;
static const uint FLAG_DEBUG_WAVE_INDEX            = 1u << 5;
static const uint FLAG_USE_EARLY_OUT               = 1u << 6;

void BendStoreOutput(RWTexture2D<float> outputTexture, int2 pixel, float visibility)
{
    uint width;
    uint height;
    outputTexture.GetDimensions(width, height);
    if (pixel.x < 0 || pixel.y < 0 || pixel.x >= int(width) || pixel.y >= int(height))
        return;

    float strength = saturate(PushConstants.Tuning.w);
    outputTexture[pixel] = lerp(1.0f, visibility, strength);
}

#define BEND_SSS_WRITE_OUTPUT(OUTPUT, COORD, VALUE) BendStoreOutput(OUTPUT, COORD, VALUE)
#include "../src/bendsss/bend_sss_gpu.h"

[[vk::binding(0, 0)]] Texture2D<float> BendDepthTexture;
[[vk::binding(1, 0)]] SamplerState BendPointBorderSampler;
[[vk::binding(2, 0)]] RWTexture2D<float> BendOutputTexture;

[numthreads(WAVE_SIZE, 1, 1)]
void main(uint3 groupID : SV_GroupID, uint groupThreadID : SV_GroupIndex)
{
    DispatchParameters parameters;
    parameters.SurfaceThickness = PushConstants.Tuning.x;
    parameters.BilinearThreshold = PushConstants.Tuning.y;
    parameters.ShadowContrast = PushConstants.Tuning.z;

    uint flags = PushConstants.Flags.x;
    parameters.IgnoreEdgePixels = (flags & FLAG_IGNORE_EDGE_PIXELS) != 0;
    parameters.UsePrecisionOffset = (flags & FLAG_USE_PRECISION_OFFSET) != 0;
    parameters.BilinearSamplingOffsetMode = (flags & FLAG_BILINEAR_SAMPLING_OFFSET) != 0;
    parameters.DebugOutputEdgeMask = (flags & FLAG_DEBUG_EDGE_MASK) != 0;
    parameters.DebugOutputThreadIndex = (flags & FLAG_DEBUG_THREAD_INDEX) != 0;
    parameters.DebugOutputWaveIndex = (flags & FLAG_DEBUG_WAVE_INDEX) != 0;
    parameters.UseEarlyOut = (flags & FLAG_USE_EARLY_OUT) != 0;

    parameters.DepthBounds = PushConstants.DepthBounds.xy;
    parameters.LightCoordinate = PushConstants.LightCoordinate;
    parameters.WaveOffset = PushConstants.WaveOffset.xy;
    parameters.FarDepthValue = PushConstants.DepthParams.z;
    parameters.NearDepthValue = PushConstants.DepthParams.w;
    parameters.InvDepthTextureSize = PushConstants.DepthParams.xy;
    parameters.DepthTexture = BendDepthTexture;
    parameters.OutputTexture = BendOutputTexture;
    parameters.PointBorderSampler = BendPointBorderSampler;

    WriteScreenSpaceShadow(parameters, int3(groupID), int(groupThreadID));
}
