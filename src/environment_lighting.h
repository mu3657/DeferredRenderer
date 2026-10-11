#pragma once

#include <vulkan/vulkan.h>
#include "vk_mem_alloc.h"
#include "vk_types.h"
#include <vector>

class VulkanEngine;

// A single linear HDR environment supplies the visible sky, indirect specular,
// and radiance for DDGI rays that leave the scene.
class EnvironmentLighting {
public:
    void init(VulkanEngine& engine);
    void prepare_frame(uint32_t frameIndex);
    void cleanup();

    VkDescriptorSet descriptor_set(uint32_t frameIndex) const { return _descriptorSets.at(frameIndex); }
    uint32_t mip_count() const { return _mipCount; }
    float intensity{0.25f};

private:
    VkDevice _device{VK_NULL_HANDLE};
    VmaAllocator _allocator{VK_NULL_HANDLE};
    VkImage _cube{VK_NULL_HANDLE};
    VmaAllocation _cubeAllocation{VK_NULL_HANDLE};
    VkImageView _cubeView{VK_NULL_HANDLE};
    VkImage _panorama{VK_NULL_HANDLE};
    VmaAllocation _panoramaAllocation{VK_NULL_HANDLE};
    VkImageView _panoramaView{VK_NULL_HANDLE};
    VkImage _brdfLut{VK_NULL_HANDLE};
    VmaAllocation _brdfAllocation{VK_NULL_HANDLE};
    VkImageView _brdfView{VK_NULL_HANDLE};
    VkSampler _sampler{VK_NULL_HANDLE};
    VkSampler _panoramaSampler{VK_NULL_HANDLE};
    std::vector<AllocatedBuffer> _parameterBuffers;
    std::vector<VkDescriptorSet> _descriptorSets;
    uint32_t _mipCount{0};
};
