#include "vk_descriptor_system.h"

#include <stdexcept>

namespace {
size_t layout_index(DescriptorLayoutID id)
{
    return static_cast<size_t>(id);
}
}

void DescriptorSystem::init(VkDevice device, uint32_t frameOverlap)
{
    _device = device;
    create_layouts();

    std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> persistentSizes = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16},
    };
    _persistentAllocator.init(_device, 64, persistentSizes);

    std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> frameSizes = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
    };

    _frameAllocators.resize(frameOverlap);
    for (auto& allocator : _frameAllocators) {
        allocator.init(_device, 128, frameSizes);
    }

    _initialized = true;
}

void DescriptorSystem::cleanup()
{
    if (!_initialized) {
        return;
    }

    for (auto& allocator : _frameAllocators) {
        allocator.destroy_pools(_device);
    }
    _frameAllocators.clear();

    _persistentAllocator.destroy_pools(_device);

    for (VkDescriptorSetLayout descriptorLayout : _layouts) {
        if (descriptorLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(_device, descriptorLayout, nullptr);
        }
    }
    _layouts.fill(VK_NULL_HANDLE);

    _device = VK_NULL_HANDLE;
    _initialized = false;
}

VkDescriptorSetLayout DescriptorSystem::layout(DescriptorLayoutID id) const
{
    return _layouts[layout_index(id)];
}

VkDescriptorSet DescriptorSystem::allocate_persistent(DescriptorLayoutID id)
{
    return _persistentAllocator.allocate(_device, layout(id));
}

VkDescriptorSet DescriptorSystem::allocate_frame(DescriptorLayoutID id, uint32_t frameIndex)
{
    if (frameIndex >= _frameAllocators.size()) {
        throw std::out_of_range("DescriptorSystem frame allocator index is out of range");
    }

    return _frameAllocators[frameIndex].allocate(_device, layout(id));
}

void DescriptorSystem::write_buffer(
    VkDescriptorSet set,
    uint32_t binding,
    VkBuffer buffer,
    size_t size,
    VkDescriptorType type,
    size_t offset)
{
    DescriptorWriter writer;
    writer.write_buffer(binding, buffer, size, offset, type);
    writer.update_set(_device, set);
}

void DescriptorSystem::write_image(
    VkDescriptorSet set,
    uint32_t binding,
    VkImageView image,
    VkSampler sampler,
    VkImageLayout imageLayout,
    VkDescriptorType type)
{
    DescriptorWriter writer;
    writer.write_image(binding, image, sampler, imageLayout, type);
    writer.update_set(_device, set);
}

void DescriptorSystem::write_image_array(
    VkDescriptorSet set,
    uint32_t binding,
    uint32_t arrayElement,
    VkImageView image,
    VkSampler sampler,
    VkImageLayout imageLayout,
    VkDescriptorType type)
{
    DescriptorWriter writer;
    writer.write_image_to_array(binding, arrayElement, image, sampler, imageLayout, type);
    writer.update_set(_device, set);
}

void DescriptorSystem::create_layouts()
{
    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        _layouts[layout_index(DescriptorLayoutID::DrawImage)] =
            builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
    }

    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        _layouts[layout_index(DescriptorLayoutID::FrameScene)] =
            builder.build(
                _device,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
    }

    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        _layouts[layout_index(DescriptorLayoutID::GBufferInput)] =
            builder.build(_device, VK_SHADER_STAGE_FRAGMENT_BIT);
    }

    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        _layouts[layout_index(DescriptorLayoutID::LightData)] =
            builder.build(_device, VK_SHADER_STAGE_FRAGMENT_BIT);
    }

    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        _layouts[layout_index(DescriptorLayoutID::ShadowInput)] =
            builder.build(_device, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT);
    }

    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        _layouts[layout_index(DescriptorLayoutID::ContactShadowCompute)] =
            builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
    }

    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        _layouts[layout_index(DescriptorLayoutID::ContactShadowInput)] =
            builder.build(_device, VK_SHADER_STAGE_FRAGMENT_BIT);
    }

    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        _layouts[layout_index(DescriptorLayoutID::GIInput)] =
            builder.build(_device, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
    }
}
