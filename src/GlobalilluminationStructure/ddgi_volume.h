#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <glm/vec3.hpp>

#include <rtxgi/ddgi/DDGIVolume.h>

#include "../vk_types.h"

class DescriptorSystem;

enum DDGIVolumeFlagBits : uint32_t {
    DDGIVolumeFlagNone = 0,
    DDGIVolumeFlagEnabled = 1u << 0,
    DDGIVolumeFlagRelocation = 1u << 1,
    DDGIVolumeFlagClassification = 1u << 2,
    DDGIVolumeFlagScrolling = 1u << 3,
    DDGIVolumeFlagResetHistory = 1u << 4,
};

using DDGIVolumeFlags = uint32_t;

struct DDGIVolumeDesc {
    // World position of probe coordinate (0, 0, 0), not the volume center.
    glm::vec3 origin{0.f};
    glm::uvec3 probeCounts{16u, 8u, 16u};
    glm::vec3 probeSpacing{1.f};

    uint32_t raysPerProbe{128};
    uint32_t probesUpdatedPerFrame{256};
    uint32_t irradianceInteriorTexels{8};
    uint32_t distanceInteriorTexels{16};

    float maxRayDistance{20.f};
    float hysteresis{0.97f};
    float normalBias{0.2f};
    float viewBias{0.1f};
    float irradianceEncodingGamma{1.f};
    float energyPreservation{1.f};
    float distanceExponent{50.f};
    float irradianceThreshold{0.25f};
    float brightnessThreshold{0.10f};
    // If at least this fraction of random rays hit back faces, the probe is
    // assumed to be inside geometry and its irradiance update is rejected.
    float randomRayBackfaceThreshold{0.1f};
    float fixedRayBackfaceThreshold{0.25f};
    float minFrontfaceDistance{1.f};

    DDGIVolumeFlags flags{DDGIVolumeFlagEnabled};
};

// std140-compatible volume constants shared with GLSL. Dynamic update scheduling
// values live here so every in-flight frame gets an immutable constants buffer.
struct alignas(16) GPUDDGIVolume {
    glm::vec4 originMaxRayDistance{};       // xyz origin, w max ray distance
    glm::vec4 spacingHysteresis{};          // xyz spacing, w hysteresis
    glm::uvec4 probeCountsRaysPerProbe{};   // xyz probe counts, w rays per probe
    glm::vec4 biasAndEncoding{};            // normal bias, view bias, gamma, energy preservation
    glm::vec4 blendThresholds{};             // x random-ray backface threshold, yzw reserved
    glm::uvec4 texelsAndUpdate{};           // irradiance texels, distance texels, first probe, probe count
    glm::ivec4 scrollOffsets{};             // xyz scrolling offset, w unused
    glm::vec4 rayRotation{0.f, 0.f, 0.f, 1.f}; // world-space quaternion
    glm::uvec4 frameAndFlags{};              // frame index, flags, total probes, unused
};

static_assert(sizeof(GPUDDGIVolume) == 144);
static_assert(offsetof(GPUDDGIVolume, originMaxRayDistance) == 0);
static_assert(offsetof(GPUDDGIVolume, spacingHysteresis) == 16);
static_assert(offsetof(GPUDDGIVolume, probeCountsRaysPerProbe) == 32);
static_assert(offsetof(GPUDDGIVolume, biasAndEncoding) == 48);
static_assert(offsetof(GPUDDGIVolume, blendThresholds) == 64);
static_assert(offsetof(GPUDDGIVolume, texelsAndUpdate) == 80);
static_assert(offsetof(GPUDDGIVolume, scrollOffsets) == 96);
static_assert(offsetof(GPUDDGIVolume, rayRotation) == 112);
static_assert(offsetof(GPUDDGIVolume, frameAndFlags) == 128);
static_assert(sizeof(rtxgi::DDGIVolumeDescGPUPacked) == 128);

struct DDGIAtlasLayout {
    uint32_t interiorTexels{0};
    uint32_t tileTexels{0};
    uint32_t tilesPerRow{0};
    uint32_t tileRowCount{0};
    uint32_t arrayLayers{0};
    VkExtent2D extent{};
};

// The owner tracks image layouts because this renderer schedules barriers manually.
struct DDGIImageResource {
    AllocatedImage image{};
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    uint32_t arrayLayers{1};
};

struct DDGIVolumeFrameResources {
    AllocatedBuffer constantsBuffer{};
    AllocatedBuffer rtxgiConstantsBuffer{};
    AllocatedBuffer diagnosticsBuffer{};
    VkDescriptorSet traceDescriptor{VK_NULL_HANDLE};
    VkDescriptorSet updateDescriptor{VK_NULL_HANDLE};
    VkDescriptorSet rtxgiUpdateDescriptor{VK_NULL_HANDLE};
    VkDescriptorSet samplingDescriptor{VK_NULL_HANDLE};
};

// Written atomically by the trace and post-blend diagnostic compute shaders.
// The frame fence makes the mapped values safe to inspect on the CPU.
struct alignas(16) DDGIDiagnosticsGPU {
    uint32_t rayCount{0};
    uint32_t nonZeroRadianceRays{0};
    uint32_t frontFaceHits{0};
    uint32_t backFaceHits{0};

    uint32_t missCount{0};
    uint32_t nonFiniteRays{0};
    uint32_t maxRadianceBits{0};
    uint32_t irradianceTexelCount{0};

    uint32_t nonZeroIrradianceTexels{0};
    uint32_t nonFiniteIrradianceTexels{0};
    uint32_t maxIrradianceBits{0};
    uint32_t reserved0{0};

    glm::uvec4 reserved1{};
};

static_assert(sizeof(DDGIDiagnosticsGPU) == 64);

struct DDGIVolumeResources {
    // RTXGI Texture2DArray layout. For right-handed Y-up, each array layer is
    // one X-Z probe plane and Y selects the layer.
    // All update resources use RGBA32F. DXC emits the official
    // RWTexture2DArray<float4> declarations as SPIR-V Rgba32f storage images,
    // so the bound Vulkan image views must use the matching format.
    // rayData: width = raysPerProbe, height = probeCountX * probeCountZ.
    DDGIImageResource rayData{};
    // One bordered octahedral tile per X-Z probe in each Y layer.
    DDGIImageResource irradiance{};
    DDGIImageResource distance{};
    // probeData: width = probeCountX, height = probeCountZ. xyz relocation
    // offset, w classification state.
    DDGIImageResource probeData{};

    VkSampler bilinearSampler{VK_NULL_HANDLE};
    std::vector<DDGIVolumeFrameResources> frames;
};

struct DDGIVolumeInitContext {
    VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VmaAllocator allocator{VK_NULL_HANDLE};
    DescriptorSystem& descriptors;
    uint32_t frameOverlap{0};
};

struct DDGIProbeUpdateRange {
    uint32_t firstProbe{0};
    uint32_t probeCount{0};
};

class DDGIVolume {
public:
    DDGIVolume() = default;
    DDGIVolume(const DDGIVolume&) = delete;
    DDGIVolume& operator=(const DDGIVolume&) = delete;

    void init(const DDGIVolumeInitContext& ctx, const DDGIVolumeDesc& desc);
    void cleanup();

    // Updates only frame-local constants and descriptors. Probe history remains shared.
    void prepare_frame(
        uint32_t frameIndex,
        uint32_t frameNumber,
        const glm::vec4& rayRotation);
    void sync_sampling_frame(uint32_t frameIndex);

    void request_history_reset();
    void set_origin(const glm::vec3& origin);
    void set_probe_spacing(const glm::vec3& spacing);
    void set_scroll_offsets(const glm::ivec3& offsets);
    void set_random_ray_backface_threshold(float threshold);

    bool initialized() const { return _initialized; }
    bool enabled() const { return (_desc.flags & DDGIVolumeFlagEnabled) != 0; }

    uint32_t total_probe_count() const;
    DDGIProbeUpdateRange update_range() const { return _updateRange; }
    uint64_t history_clear_serial() const { return _historyClearSerial; }
    uint32_t history_probe_count() const { return _historyProbeCount; }

    const DDGIVolumeDesc& desc() const { return _desc; }
    const GPUDDGIVolume& gpu_data() const { return _gpuData; }
    // Canonical RTXGI 1.3.6 packed ABI. The active GLSL passes still consume
    // gpu_data() until their resources move to Texture2DArray.
    const rtxgi::DDGIVolumeDescGPUPacked& rtxgi_gpu_data() const {
        return _rtxgiGpuData;
    }
    const DDGIAtlasLayout& irradiance_layout() const { return _irradianceLayout; }
    const DDGIAtlasLayout& distance_layout() const { return _distanceLayout; }

    DDGIVolumeResources& resources() { return _resources; }
    const DDGIVolumeResources& resources() const { return _resources; }

    VkDescriptorSet trace_descriptor_set(uint32_t frameIndex) const;
    VkDescriptorSet update_descriptor_set(uint32_t frameIndex) const;
    VkDescriptorSet rtxgi_update_descriptor_set(uint32_t frameIndex) const;
    VkDescriptorSet sampling_descriptor_set(uint32_t frameIndex) const;

private:
    VkPhysicalDevice _physicalDevice{VK_NULL_HANDLE};
    VkDevice _device{VK_NULL_HANDLE};
    VmaAllocator _allocator{VK_NULL_HANDLE};
    DescriptorSystem* _descriptors{nullptr};

    DDGIVolumeDesc _desc{};
    GPUDDGIVolume _gpuData{};
    rtxgi::DDGIVolumeDescGPUPacked _rtxgiGpuData{};
    DDGIAtlasLayout _irradianceLayout{};
    DDGIAtlasLayout _distanceLayout{};
    DDGIVolumeResources _resources{};
    DDGIProbeUpdateRange _updateRange{};
    glm::ivec3 _scrollOffsets{0};

    uint32_t _nextProbeToUpdate{0};
    uint32_t _historyProbeCount{0};
    uint64_t _historyClearSerial{1};
    bool _historyResetRequested{true};
    bool _initialized{false};

    void upload_constants(uint32_t frameIndex);
    void update_rtxgi_gpu_data(const glm::vec4& rayRotation);
};
