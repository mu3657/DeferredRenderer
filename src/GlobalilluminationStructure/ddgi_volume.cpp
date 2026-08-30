#include "GlobalilluminationStructure/ddgi_volume.h"

#include "vk_descriptor_system.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {

uint32_t checked_probe_count(const glm::uvec3& counts)
{
    if (counts.x == 0 || counts.y == 0 || counts.z == 0) {
        throw std::invalid_argument("DDGI probe counts must be non-zero");
    }

    const uint64_t total = static_cast<uint64_t>(counts.x)
        * static_cast<uint64_t>(counts.y)
        * static_cast<uint64_t>(counts.z);
    if (total > std::numeric_limits<uint32_t>::max()) {
        throw std::overflow_error("DDGI probe count exceeds uint32_t");
    }
    return static_cast<uint32_t>(total);
}

DDGIAtlasLayout make_atlas_layout(
    const glm::uvec3& probeCounts,
    uint32_t interiorTexels)
{
    if (interiorTexels == 0) {
        throw std::invalid_argument("DDGI atlas interior texel count must be non-zero");
    }

    DDGIAtlasLayout layout{};
    layout.interiorTexels = interiorTexels;
    layout.tileTexels = interiorTexels + 2;
    layout.tilesPerRow = probeCounts.x;
    layout.tileRowCount = probeCounts.z;
    layout.arrayLayers = probeCounts.y;
    layout.extent = {
        layout.tilesPerRow * layout.tileTexels,
        layout.tileRowCount * layout.tileTexels,
    };
    return layout;
}

void validate_storage_image_format(VkPhysicalDevice physicalDevice, VkFormat format)
{
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
    constexpr VkFormatFeatureFlags Required =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((properties.optimalTilingFeatures & Required) != Required) {
        throw std::runtime_error("GPU does not support a required DDGI storage image format");
    }
}

AllocatedImage create_image(
    VkDevice device,
    VmaAllocator allocator,
    VkExtent2D extent,
    uint32_t arrayLayers,
    VkFormat format)
{
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = arrayLayers;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT
        | VK_IMAGE_USAGE_SAMPLED_BIT
        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
        | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    AllocatedImage image{};
    image.imageExtent = imageInfo.extent;
    image.imageFormat = format;
    VK_CHECK(vmaCreateImage(
        allocator,
        &imageInfo,
        &allocationInfo,
        &image.image,
        &image.allocation,
        nullptr));

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = arrayLayers;
    VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &image.imageView));
    return image;
}

AllocatedBuffer create_constants_buffer(VmaAllocator allocator)
{
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = sizeof(GPUDDGIVolume);
    bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    allocationInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    AllocatedBuffer buffer{};
    VK_CHECK(vmaCreateBuffer(
        allocator,
        &bufferInfo,
        &allocationInfo,
        &buffer.buffer,
        &buffer.allocation,
        &buffer.info));
    return buffer;
}

AllocatedBuffer create_rtxgi_constants_buffer(VmaAllocator allocator)
{
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = sizeof(rtxgi::DDGIVolumeDescGPUPacked);
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    allocationInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    AllocatedBuffer buffer{};
    VK_CHECK(vmaCreateBuffer(
        allocator,
        &bufferInfo,
        &allocationInfo,
        &buffer.buffer,
        &buffer.allocation,
        &buffer.info));
    return buffer;
}

AllocatedBuffer create_diagnostics_buffer(VmaAllocator allocator)
{
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = sizeof(DDGIDiagnosticsGPU);
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
    allocationInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    AllocatedBuffer buffer{};
    VK_CHECK(vmaCreateBuffer(
        allocator,
        &bufferInfo,
        &allocationInfo,
        &buffer.buffer,
        &buffer.allocation,
        &buffer.info));
    return buffer;
}

void destroy_image(VkDevice device, VmaAllocator allocator, DDGIImageResource& resource)
{
    if (resource.image.imageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, resource.image.imageView, nullptr);
    }
    if (resource.image.image != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator, resource.image.image, resource.image.allocation);
    }
    resource = {};
}

} // namespace

void DDGIVolume::init(const DDGIVolumeInitContext& ctx, const DDGIVolumeDesc& desc)
{
    if (_initialized) {
        throw std::logic_error("DDGIVolume is already initialized");
    }
    if (ctx.physicalDevice == VK_NULL_HANDLE
        || ctx.device == VK_NULL_HANDLE
        || ctx.allocator == VK_NULL_HANDLE
        || ctx.frameOverlap == 0) {
        throw std::invalid_argument("DDGIVolumeInitContext is incomplete");
    }
    if (desc.raysPerProbe == 0
        || desc.probesUpdatedPerFrame == 0
        || desc.maxRayDistance <= 0.f
        || desc.randomRayBackfaceThreshold < 0.f
        || desc.randomRayBackfaceThreshold > 1.f
        || desc.probeSpacing.x <= 0.f
        || desc.probeSpacing.y <= 0.f
        || desc.probeSpacing.z <= 0.f) {
        throw std::invalid_argument("DDGIVolumeDesc contains invalid trace dimensions");
    }

    const uint32_t probeCount = checked_probe_count(desc.probeCounts);
    _irradianceLayout = make_atlas_layout(desc.probeCounts, desc.irradianceInteriorTexels);
    _distanceLayout = make_atlas_layout(desc.probeCounts, desc.distanceInteriorTexels);

    VkPhysicalDeviceProperties deviceProperties{};
    vkGetPhysicalDeviceProperties(ctx.physicalDevice, &deviceProperties);
    const uint32_t maxImageDimension = deviceProperties.limits.maxImageDimension2D;
    const auto validateExtent = [maxImageDimension](VkExtent2D extent) {
        if (extent.width == 0 || extent.height == 0
            || extent.width > maxImageDimension || extent.height > maxImageDimension) {
            throw std::out_of_range("DDGI image extent exceeds maxImageDimension2D");
        }
    };
    const uint32_t probesPerLayer = desc.probeCounts.x * desc.probeCounts.z;
    validateExtent({desc.raysPerProbe, probesPerLayer});
    validateExtent(_irradianceLayout.extent);
    validateExtent(_distanceLayout.extent);
    validateExtent({desc.probeCounts.x, desc.probeCounts.z});

    validate_storage_image_format(ctx.physicalDevice, VK_FORMAT_R32G32B32A32_SFLOAT);

    _physicalDevice = ctx.physicalDevice;
    _device = ctx.device;
    _allocator = ctx.allocator;
    _descriptors = &ctx.descriptors;
    _desc = desc;
    _resources.frames.resize(ctx.frameOverlap);
    _initialized = true;

    try {
        _resources.rayData.image = create_image(
            _device, _allocator, {desc.raysPerProbe, probesPerLayer}, desc.probeCounts.y,
            VK_FORMAT_R32G32B32A32_SFLOAT);
        _resources.rayData.arrayLayers = desc.probeCounts.y;
        _resources.irradiance.image = create_image(
            _device, _allocator, _irradianceLayout.extent, _irradianceLayout.arrayLayers,
            VK_FORMAT_R32G32B32A32_SFLOAT);
        _resources.irradiance.arrayLayers = _irradianceLayout.arrayLayers;
        _resources.distance.image = create_image(
            _device, _allocator, _distanceLayout.extent, _distanceLayout.arrayLayers,
            VK_FORMAT_R32G32B32A32_SFLOAT);
        _resources.distance.arrayLayers = _distanceLayout.arrayLayers;
        _resources.probeData.image = create_image(
            _device, _allocator, {desc.probeCounts.x, desc.probeCounts.z}, desc.probeCounts.y,
            VK_FORMAT_R32G32B32A32_SFLOAT);
        _resources.probeData.arrayLayers = desc.probeCounts.y;

        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = 0.f;
        VK_CHECK(vkCreateSampler(_device, &samplerInfo, nullptr, &_resources.bilinearSampler));

        _gpuData.originMaxRayDistance = glm::vec4(_desc.origin, _desc.maxRayDistance);
        _gpuData.spacingHysteresis = glm::vec4(_desc.probeSpacing, _desc.hysteresis);
        _gpuData.probeCountsRaysPerProbe = glm::uvec4(_desc.probeCounts, _desc.raysPerProbe);
        _gpuData.biasAndEncoding = glm::vec4(
            _desc.normalBias,
            _desc.viewBias,
            _desc.irradianceEncodingGamma,
            _desc.energyPreservation);
        _gpuData.blendThresholds = glm::vec4(
            _desc.randomRayBackfaceThreshold, 0.f, 0.f, 0.f);
        _gpuData.texelsAndUpdate = glm::uvec4(
            _desc.irradianceInteriorTexels,
            _desc.distanceInteriorTexels,
            0u,
            0u);
        _gpuData.rayRotation = glm::vec4(0.f, 0.f, 0.f, 1.f);
        _gpuData.frameAndFlags = glm::uvec4(
            0u,
            _desc.flags | DDGIVolumeFlagResetHistory,
            probeCount,
            0u);
        update_rtxgi_gpu_data(_gpuData.rayRotation);

        for (uint32_t frameIndex = 0;
             frameIndex < static_cast<uint32_t>(_resources.frames.size());
             ++frameIndex) {
            DDGIVolumeFrameResources& frame = _resources.frames[frameIndex];
            frame.constantsBuffer = create_constants_buffer(_allocator);
            frame.rtxgiConstantsBuffer = create_rtxgi_constants_buffer(_allocator);
            frame.diagnosticsBuffer = create_diagnostics_buffer(_allocator);
            std::memset(
                frame.diagnosticsBuffer.info.pMappedData,
                0,
                sizeof(DDGIDiagnosticsGPU));
            frame.traceDescriptor =
                _descriptors->allocate_persistent(DescriptorLayoutID::DDGIProbeTrace);
            _descriptors->write_buffer(
                frame.traceDescriptor,
                1,
                frame.constantsBuffer.buffer,
                sizeof(GPUDDGIVolume),
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
            _descriptors->write_image(
                frame.traceDescriptor,
                2,
                _resources.rayData.image.imageView,
                VK_NULL_HANDLE,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            _descriptors->write_image(
                frame.traceDescriptor,
                5,
                _resources.irradiance.image.imageView,
                _resources.bilinearSampler,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            _descriptors->write_image(
                frame.traceDescriptor,
                6,
                _resources.distance.image.imageView,
                _resources.bilinearSampler,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            _descriptors->write_buffer(
                frame.traceDescriptor,
                7,
                frame.diagnosticsBuffer.buffer,
                sizeof(DDGIDiagnosticsGPU),
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

            frame.updateDescriptor =
                _descriptors->allocate_persistent(DescriptorLayoutID::DDGIProbeBlend);
            _descriptors->write_buffer(
                frame.updateDescriptor,
                0,
                frame.constantsBuffer.buffer,
                sizeof(GPUDDGIVolume),
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
            _descriptors->write_image(
                frame.updateDescriptor,
                1,
                _resources.rayData.image.imageView,
                VK_NULL_HANDLE,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);

            frame.rtxgiUpdateDescriptor =
                _descriptors->allocate_persistent(DescriptorLayoutID::DDGIProbeBlendRTXGI);
            _descriptors->write_buffer(
                frame.rtxgiUpdateDescriptor,
                0,
                frame.rtxgiConstantsBuffer.buffer,
                sizeof(rtxgi::DDGIVolumeDescGPUPacked),
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            _descriptors->write_image(
                frame.rtxgiUpdateDescriptor,
                1,
                _resources.rayData.image.imageView,
                VK_NULL_HANDLE,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            _descriptors->write_image(
                frame.rtxgiUpdateDescriptor,
                2,
                _resources.irradiance.image.imageView,
                VK_NULL_HANDLE,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            _descriptors->write_image(
                frame.rtxgiUpdateDescriptor,
                3,
                _resources.probeData.image.imageView,
                VK_NULL_HANDLE,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            // Variability is disabled in the packed constants for now. Bind a
            // valid storage view so the official shader ABI is complete.
            _descriptors->write_image(
                frame.rtxgiUpdateDescriptor,
                4,
                _resources.probeData.image.imageView,
                VK_NULL_HANDLE,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            _descriptors->write_image(
                frame.rtxgiUpdateDescriptor,
                5,
                _resources.distance.image.imageView,
                VK_NULL_HANDLE,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            _descriptors->write_buffer(
                frame.rtxgiUpdateDescriptor,
                6,
                frame.diagnosticsBuffer.buffer,
                sizeof(DDGIDiagnosticsGPU),
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            _descriptors->write_image(
                frame.updateDescriptor,
                2,
                _resources.irradiance.image.imageView,
                VK_NULL_HANDLE,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            _descriptors->write_image(
                frame.updateDescriptor,
                3,
                _resources.distance.image.imageView,
                VK_NULL_HANDLE,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);

            frame.samplingDescriptor =
                _descriptors->allocate_persistent(DescriptorLayoutID::GIInput);
            _descriptors->write_buffer(
                frame.samplingDescriptor,
                0,
                frame.constantsBuffer.buffer,
                sizeof(GPUDDGIVolume),
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
            _descriptors->write_image(
                frame.samplingDescriptor,
                1,
                _resources.irradiance.image.imageView,
                _resources.bilinearSampler,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            _descriptors->write_image(
                frame.samplingDescriptor,
                2,
                _resources.distance.image.imageView,
                _resources.bilinearSampler,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            upload_constants(frameIndex);
        }
    } catch (...) {
        cleanup();
        throw;
    }
}

void DDGIVolume::cleanup()
{
    if (!_initialized) {
        return;
    }

    VK_CHECK(vkDeviceWaitIdle(_device));
    for (DDGIVolumeFrameResources& frame : _resources.frames) {
        if (frame.constantsBuffer.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(_allocator, frame.constantsBuffer.buffer, frame.constantsBuffer.allocation);
        }
        if (frame.rtxgiConstantsBuffer.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(
                _allocator,
                frame.rtxgiConstantsBuffer.buffer,
                frame.rtxgiConstantsBuffer.allocation);
        }
        if (frame.diagnosticsBuffer.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(
                _allocator,
                frame.diagnosticsBuffer.buffer,
                frame.diagnosticsBuffer.allocation);
        }
        frame = {};
    }
    _resources.frames.clear();

    if (_resources.bilinearSampler != VK_NULL_HANDLE) {
        vkDestroySampler(_device, _resources.bilinearSampler, nullptr);
    }
    destroy_image(_device, _allocator, _resources.rayData);
    destroy_image(_device, _allocator, _resources.irradiance);
    destroy_image(_device, _allocator, _resources.distance);
    destroy_image(_device, _allocator, _resources.probeData);

    _resources = {};
    _gpuData = {};
    _rtxgiGpuData = {};
    _updateRange = {};
    _physicalDevice = VK_NULL_HANDLE;
    _device = VK_NULL_HANDLE;
    _allocator = VK_NULL_HANDLE;
    _descriptors = nullptr;
    _nextProbeToUpdate = 0;
    _historyProbeCount = 0;
    _historyClearSerial = 1;
    _historyResetRequested = true;
    _initialized = false;
}

void DDGIVolume::prepare_frame(
    uint32_t frameIndex,
    uint32_t frameNumber,
    const glm::vec4& rayRotation)
{
    if (!_initialized || frameIndex >= _resources.frames.size()) {
        throw std::out_of_range("DDGIVolume frame index is out of range");
    }

    const uint32_t probeCount = total_probe_count();
    const uint32_t updateCount = std::min(_desc.probesUpdatedPerFrame, probeCount);
    if (_nextProbeToUpdate >= probeCount) {
        _nextProbeToUpdate = 0;
    }
    _updateRange.firstProbe = _nextProbeToUpdate;
    _updateRange.probeCount = std::min(updateCount, probeCount - _nextProbeToUpdate);

    _gpuData.originMaxRayDistance = glm::vec4(_desc.origin, _desc.maxRayDistance);
    _gpuData.spacingHysteresis = glm::vec4(_desc.probeSpacing, _desc.hysteresis);
    _gpuData.probeCountsRaysPerProbe = glm::uvec4(_desc.probeCounts, _desc.raysPerProbe);
    _gpuData.biasAndEncoding = glm::vec4(
        _desc.normalBias,
        _desc.viewBias,
        _desc.irradianceEncodingGamma,
        _desc.energyPreservation);
    _gpuData.blendThresholds = glm::vec4(
        _desc.randomRayBackfaceThreshold, 0.f, 0.f, 0.f);
    _gpuData.texelsAndUpdate = glm::uvec4(
        _desc.irradianceInteriorTexels,
        _desc.distanceInteriorTexels,
        _updateRange.firstProbe,
        _updateRange.probeCount);
    _gpuData.scrollOffsets = glm::ivec4(_scrollOffsets, 0);
    _gpuData.rayRotation = rayRotation;
    DDGIVolumeFlags frameFlags = _desc.flags;
    if (_historyResetRequested) {
        frameFlags |= DDGIVolumeFlagResetHistory;
    }
    _gpuData.frameAndFlags = glm::uvec4(frameNumber, frameFlags, probeCount, 0u);
    update_rtxgi_gpu_data(rayRotation);

    DDGIVolumeFrameResources& frame = _resources.frames[frameIndex];
    std::memset(
        frame.diagnosticsBuffer.info.pMappedData,
        0,
        sizeof(DDGIDiagnosticsGPU));
    VK_CHECK(vmaFlushAllocation(
        _allocator,
        frame.diagnosticsBuffer.allocation,
        0,
        sizeof(DDGIDiagnosticsGPU)));

    upload_constants(frameIndex);

    _nextProbeToUpdate += _updateRange.probeCount;
    _historyProbeCount = std::min(
        probeCount, _historyProbeCount + _updateRange.probeCount);
    if (_nextProbeToUpdate >= probeCount) {
        _nextProbeToUpdate = 0;
        _historyResetRequested = false;
    }
}

void DDGIVolume::sync_sampling_frame(uint32_t frameIndex)
{
    upload_constants(frameIndex);
}

void DDGIVolume::upload_constants(uint32_t frameIndex)
{
    if (!_initialized || frameIndex >= _resources.frames.size()) {
        throw std::out_of_range("DDGIVolume constants frame index is out of range");
    }

    DDGIVolumeFrameResources& frame = _resources.frames[frameIndex];
    std::memcpy(frame.constantsBuffer.info.pMappedData, &_gpuData, sizeof(_gpuData));
    VK_CHECK(vmaFlushAllocation(
        _allocator, frame.constantsBuffer.allocation, 0, sizeof(_gpuData)));
    std::memcpy(
        frame.rtxgiConstantsBuffer.info.pMappedData,
        &_rtxgiGpuData,
        sizeof(_rtxgiGpuData));
    VK_CHECK(vmaFlushAllocation(
        _allocator,
        frame.rtxgiConstantsBuffer.allocation,
        0,
        sizeof(_rtxgiGpuData)));
}

void DDGIVolume::update_rtxgi_gpu_data(const glm::vec4& rayRotation)
{
    rtxgi::DDGIVolumeDescGPU gpu{};

    // RTXGI defines origin as the volume center. DeferredRenderer's legacy
    // descriptor defines it as probe coordinate (0, 0, 0), so adapt explicitly.
    const glm::vec3 halfGridExtent = _desc.probeSpacing
        * (glm::vec3(_desc.probeCounts) - glm::vec3(1.f)) * 0.5f;
    const glm::vec3 center = _desc.origin + halfGridExtent;
    gpu.origin = {center.x, center.y, center.z};
    gpu.rotation = {0.f, 0.f, 0.f, 1.f};
    gpu.probeRayRotation = {
        rayRotation.x, rayRotation.y, rayRotation.z, rayRotation.w};
    gpu.movementType = (_desc.flags & DDGIVolumeFlagScrolling) != 0
        ? static_cast<uint32_t>(rtxgi::EDDGIVolumeMovementType::Scrolling)
        : static_cast<uint32_t>(rtxgi::EDDGIVolumeMovementType::Default);
    gpu.probeSpacing = {
        _desc.probeSpacing.x, _desc.probeSpacing.y, _desc.probeSpacing.z};
    gpu.probeCounts = {
        static_cast<int>(_desc.probeCounts.x),
        static_cast<int>(_desc.probeCounts.y),
        static_cast<int>(_desc.probeCounts.z)};
    gpu.probeNumRays = static_cast<int>(_desc.raysPerProbe);
    gpu.probeNumIrradianceInteriorTexels =
        static_cast<int>(_desc.irradianceInteriorTexels);
    gpu.probeNumDistanceInteriorTexels =
        static_cast<int>(_desc.distanceInteriorTexels);
    gpu.probeHysteresis = _desc.hysteresis;
    gpu.probeMaxRayDistance = _desc.maxRayDistance;
    gpu.probeNormalBias = _desc.normalBias;
    gpu.probeViewBias = _desc.viewBias;
    gpu.probeDistanceExponent = _desc.distanceExponent;
    gpu.probeIrradianceEncodingGamma = _desc.irradianceEncodingGamma;
    gpu.probeIrradianceThreshold = _desc.irradianceThreshold;
    gpu.probeBrightnessThreshold = _desc.brightnessThreshold;
    gpu.probeRandomRayBackfaceThreshold = _desc.randomRayBackfaceThreshold;
    gpu.probeFixedRayBackfaceThreshold = _desc.fixedRayBackfaceThreshold;
    gpu.probeMinFrontfaceDistance = _desc.minFrontfaceDistance;
    gpu.probeScrollOffsets = {
        _scrollOffsets.x, _scrollOffsets.y, _scrollOffsets.z};
    gpu.probeRayDataFormat =
        static_cast<uint32_t>(rtxgi::EDDGIVolumeTextureFormat::F32x4);
    gpu.probeIrradianceFormat =
        static_cast<uint32_t>(rtxgi::EDDGIVolumeTextureFormat::F32x4);
    gpu.probeRelocationEnabled =
        (_desc.flags & DDGIVolumeFlagRelocation) != 0;
    gpu.probeClassificationEnabled =
        (_desc.flags & DDGIVolumeFlagClassification) != 0;
    gpu.probeVariabilityEnabled = false;

    _rtxgiGpuData = rtxgi::PackDDGIVolumeDescGPU(gpu);
}

void DDGIVolume::request_history_reset()
{
    _historyResetRequested = true;
    _nextProbeToUpdate = 0;
    _historyProbeCount = 0;
    ++_historyClearSerial;
}

void DDGIVolume::set_origin(const glm::vec3& origin)
{
    if (_desc.origin != origin) {
        _desc.origin = origin;
        _gpuData.originMaxRayDistance = glm::vec4(origin, _desc.maxRayDistance);
        request_history_reset();
    }
}

void DDGIVolume::set_probe_spacing(const glm::vec3& spacing)
{
    if (spacing.x <= 0.f || spacing.y <= 0.f || spacing.z <= 0.f) {
        throw std::invalid_argument("DDGI probe spacing must be positive");
    }
    if (_desc.probeSpacing != spacing) {
        _desc.probeSpacing = spacing;
        _gpuData.spacingHysteresis = glm::vec4(spacing, _desc.hysteresis);
        request_history_reset();
    }
}

void DDGIVolume::set_scroll_offsets(const glm::ivec3& offsets)
{
    if (_scrollOffsets != offsets) {
        _scrollOffsets = offsets;
        request_history_reset();
    }
}

void DDGIVolume::set_random_ray_backface_threshold(float threshold)
{
    if (threshold < 0.f || threshold > 1.f) {
        throw std::invalid_argument(
            "DDGI random-ray backface threshold must be in [0, 1]");
    }
    if (_desc.randomRayBackfaceThreshold != threshold) {
        _desc.randomRayBackfaceThreshold = threshold;
        _gpuData.blendThresholds.x = threshold;
        request_history_reset();
    }
}

uint32_t DDGIVolume::total_probe_count() const
{
    return checked_probe_count(_desc.probeCounts);
}

VkDescriptorSet DDGIVolume::trace_descriptor_set(uint32_t frameIndex) const
{
    if (frameIndex >= _resources.frames.size()) {
        throw std::out_of_range("DDGIVolume trace descriptor frame index is out of range");
    }
    return _resources.frames[frameIndex].traceDescriptor;
}

VkDescriptorSet DDGIVolume::update_descriptor_set(uint32_t frameIndex) const
{
    if (frameIndex >= _resources.frames.size()) {
        throw std::out_of_range("DDGIVolume update descriptor frame index is out of range");
    }
    return _resources.frames[frameIndex].updateDescriptor;
}

VkDescriptorSet DDGIVolume::rtxgi_update_descriptor_set(uint32_t frameIndex) const
{
    if (frameIndex >= _resources.frames.size()) {
        throw std::out_of_range("DDGIVolume RTXGI update descriptor frame index is out of range");
    }
    return _resources.frames[frameIndex].rtxgiUpdateDescriptor;
}

VkDescriptorSet DDGIVolume::sampling_descriptor_set(uint32_t frameIndex) const
{
    if (frameIndex >= _resources.frames.size()) {
        throw std::out_of_range("DDGIVolume sampling descriptor frame index is out of range");
    }
    return _resources.frames[frameIndex].samplingDescriptor;
}
