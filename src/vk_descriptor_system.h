#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "vk_descriptors.h"

enum class DescriptorLayoutID : uint8_t {
    DrawImage,
    FrameScene,
    GBufferInput,
    LightData,
    ShadowInput,
    ContactShadowCompute,
    ContactShadowInput,
    DDGIProbeTrace,
    DDGIProbeBlend,
    DDGIProbeBlendRTXGI,
    GIInput,
    Count
};

class DescriptorSystem {
public:
    void init(VkDevice device, uint32_t frameOverlap);
    void cleanup();

    VkDescriptorSetLayout layout(DescriptorLayoutID id) const;

    VkDescriptorSet allocate_persistent(DescriptorLayoutID id);
    VkDescriptorSet allocate_frame(DescriptorLayoutID id, uint32_t frameIndex);

    void write_buffer(
        VkDescriptorSet set,
        uint32_t binding,
        VkBuffer buffer,
        size_t size,
        VkDescriptorType type,
        size_t offset = 0);

    void write_image(
        VkDescriptorSet set,
        uint32_t binding,
        VkImageView image,
        VkSampler sampler,
        VkImageLayout imageLayout,
        VkDescriptorType type);

    void write_image_array(
        VkDescriptorSet set,
        uint32_t binding,
        uint32_t arrayElement,
        VkImageView image,
        VkSampler sampler,
        VkImageLayout imageLayout,
        VkDescriptorType type);

    void write_acceleration_structure(
        VkDescriptorSet set,
        uint32_t binding,
        VkAccelerationStructureKHR accelerationStructure);

private:
    static constexpr size_t LayoutCount = static_cast<size_t>(DescriptorLayoutID::Count);

    VkDevice _device{VK_NULL_HANDLE};
    std::array<VkDescriptorSetLayout, LayoutCount> _layouts{};
    DescriptorAllocatorGrowable _persistentAllocator;
    std::vector<DescriptorAllocatorGrowable> _frameAllocators;
    bool _initialized{false};

    void create_layouts();
};
