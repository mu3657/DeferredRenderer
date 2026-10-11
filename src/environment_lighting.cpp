#include "environment_lighting.h"

#include "vk_engine.h"
#include "stb_image.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace {
constexpr uint32_t CubeSize = 128;
constexpr uint32_t LutSize = 128;
constexpr float Pi = std::numbers::pi_v<float>;
struct alignas(16) GPUEnvironmentParameters {
    glm::vec4 intensity;
};
static_assert(sizeof(GPUEnvironmentParameters) == 16);

struct Panorama {
    int width{0};
    int height{0};
    std::vector<glm::vec3> pixels;

    glm::vec3 sample(glm::vec3 direction) const {
        direction = glm::normalize(direction);
        if (pixels.empty()) {
            // A directional HDR fallback keeps sky, reflections and probe misses
            // coherent even when no external environment.hdr has been supplied.
            const float up = glm::clamp(direction.y, 0.f, 1.f);
            const glm::vec3 horizon(0.42f, 0.53f, 0.72f);
            const glm::vec3 zenith(0.11f, 0.25f, 0.55f);
            const glm::vec3 ground(0.08f, 0.075f, 0.07f);
            const glm::vec3 sky = glm::mix(horizon, zenith, std::pow(up, 0.65f));
            const float sun = std::pow(glm::max(glm::dot(direction,
                glm::normalize(glm::vec3(0.35f, 0.85f, 0.24f))), 0.f), 512.f);
            return (direction.y < 0.f ? ground : sky) + glm::vec3(8.f, 6.7f, 5.f) * sun;
        }
        const float u = std::atan2(direction.z, direction.x) / (2.f * Pi) + 0.5f;
        const float v = std::acos(glm::clamp(direction.y, -1.f, 1.f)) / Pi;
        const float x = u * float(width) - 0.5f;
        const float y = v * float(height) - 0.5f;
        const int x0 = static_cast<int>(std::floor(x));
        const int y0 = std::clamp(static_cast<int>(std::floor(y)), 0, height - 1);
        const int y1 = std::min(y0 + 1, height - 1);
        const float fx = x - std::floor(x);
        const float fy = glm::clamp(y - std::floor(y), 0.f, 1.f);
        const auto pixel = [&](int px, int py) {
            const int wrapped = ((px % width) + width) % width;
            return pixels[size_t(py) * width + wrapped];
        };
        return glm::mix(glm::mix(pixel(x0, y0), pixel(x0 + 1, y0), fx),
            glm::mix(pixel(x0, y1), pixel(x0 + 1, y1), fx), fy);
    }
};

Panorama load_panorama() {
    Panorama result;
    for (const char* path : {
            "../assets/environment.hdr", "assets/environment.hdr",
            "../assets/rogland_clear_night_2k.hdr", "assets/rogland_clear_night_2k.hdr"}) {
        if (!std::filesystem::exists(path)) continue;
        int channels = 0;
        float* data = stbi_loadf(path, &result.width, &result.height, &channels, 3);
        if (!data || result.width <= 0 || result.height <= 0) {
            if (data) stbi_image_free(data);
            fmt::println("Environment: failed to load {}; using procedural HDR sky", path);
            result = {};
            return result;
        }
        result.pixels.resize(size_t(result.width) * result.height);
        for (size_t i = 0; i < result.pixels.size(); ++i) {
            result.pixels[i] = glm::max(glm::vec3(data[3*i], data[3*i+1], data[3*i+2]), glm::vec3(0.f));
        }
        stbi_image_free(data);
        fmt::println("Environment: loaded {} ({}x{})", path, result.width, result.height);
        return result;
    }
    fmt::println("Environment: using procedural HDR sky; place environment.hdr in assets to replace it");
    return result;
}

glm::vec3 cube_direction(uint32_t face, float u, float v) {
    switch (face) {
    case 0: return glm::normalize(glm::vec3( 1.f, -v, -u));
    case 1: return glm::normalize(glm::vec3(-1.f, -v,  u));
    case 2: return glm::normalize(glm::vec3( u,  1.f,  v));
    case 3: return glm::normalize(glm::vec3( u, -1.f, -v));
    case 4: return glm::normalize(glm::vec3( u, -v,  1.f));
    default:return glm::normalize(glm::vec3(-u, -v, -1.f));
    }
}

float radical_inverse(uint32_t bits) {
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xaaaaaaaau) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xccccccccu) >> 2);
    bits = ((bits & 0x0f0f0f0fu) << 4) | ((bits & 0xf0f0f0f0u) >> 4);
    bits = ((bits & 0x00ff00ffu) << 8) | ((bits & 0xff00ff00u) >> 8);
    return float(bits) * 2.3283064365386963e-10f;
}

glm::vec3 importance_sample_ggx(uint32_t i, uint32_t count, float roughness, glm::vec3 normal) {
    const float a = std::max(roughness * roughness, 0.0016f);
    const float phi = 2.f * Pi * radical_inverse(i);
    const float xi = (float(i) + 0.5f) / float(count);
    const float cosTheta = std::sqrt((1.f - xi) / (1.f + (a * a - 1.f) * xi));
    const float sinTheta = std::sqrt(std::max(1.f - cosTheta * cosTheta, 0.f));
    const glm::vec3 h(std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta);
    const glm::vec3 up = std::abs(normal.z) < 0.999f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
    const glm::vec3 tangent = glm::normalize(glm::cross(up, normal));
    return glm::normalize(tangent * h.x + glm::cross(normal, tangent) * h.y + normal * h.z);
}

float smith_g1(float noX, float a) {
    const float a2 = a * a;
    return 2.f * noX / std::max(noX + std::sqrt(a2 + (1.f - a2) * noX * noX), 1e-5f);
}

void create_image(VkDevice device, VmaAllocator allocator, uint32_t width, uint32_t height, uint32_t mipCount,
    uint32_t layers, VkFormat format, VkImageCreateFlags flags, VkImage& image,
    VmaAllocation& allocation, VkImageView& view, VkImageViewType viewType) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = flags;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = mipCount;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    VK_CHECK(vmaCreateImage(allocator, &info, &allocationInfo, &image, &allocation, nullptr));
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = viewType;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = mipCount;
    viewInfo.subresourceRange.layerCount = layers;
    VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &view));
}

void upload(VulkanEngine& engine, VkImage image, uint32_t mips, uint32_t layers,
    const std::vector<float>& pixels, const std::vector<VkBufferImageCopy>& regions) {
    const VkDeviceSize byteCount = VkDeviceSize(pixels.size()) * sizeof(float);
    AllocatedBuffer staging = engine.create_buffer(byteCount, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);
    std::memcpy(staging.info.pMappedData, pixels.data(), size_t(byteCount));
    engine.immediate_submit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = mips;
        barrier.subresourceRange.layerCount = layers;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
        vkCmdCopyBufferToImage(cmd, staging.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            uint32_t(regions.size()), regions.data());
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &dependency);
    });
    engine.destroy_buffer(staging);
}
} // namespace

void EnvironmentLighting::init(VulkanEngine& engine) {
    _device = engine._device;
    _allocator = engine._allocator;
    const Panorama panorama = load_panorama();
    _mipCount = 8;

    const uint32_t panoramaWidth = panorama.pixels.empty() ? 1024u : uint32_t(panorama.width);
    const uint32_t panoramaHeight = panorama.pixels.empty() ? 512u : uint32_t(panorama.height);
    std::vector<float> panoramaPixels;
    panoramaPixels.reserve(size_t(panoramaWidth) * panoramaHeight * 4);
    for (uint32_t y = 0; y < panoramaHeight; ++y) for (uint32_t x = 0; x < panoramaWidth; ++x) {
        glm::vec3 color;
        if (panorama.pixels.empty()) {
            const float u = (float(x) + 0.5f) / float(panoramaWidth);
            const float v = (float(y) + 0.5f) / float(panoramaHeight);
            const float phi = (u - 0.5f) * 2.f * Pi;
            const float theta = v * Pi;
            color = panorama.sample(glm::vec3(
                std::cos(phi) * std::sin(theta), std::cos(theta),
                std::sin(phi) * std::sin(theta)));
        } else {
            color = panorama.pixels[size_t(y) * panoramaWidth + x];
        }
        panoramaPixels.insert(panoramaPixels.end(), {color.r, color.g, color.b, 1.f});
    }
    create_image(_device, _allocator, panoramaWidth, panoramaHeight, 1, 1,
        VK_FORMAT_R32G32B32A32_SFLOAT, 0, _panorama, _panoramaAllocation,
        _panoramaView, VK_IMAGE_VIEW_TYPE_2D);
    VkBufferImageCopy panoramaRegion{};
    panoramaRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    panoramaRegion.imageSubresource.layerCount = 1;
    panoramaRegion.imageExtent = {panoramaWidth, panoramaHeight, 1};
    upload(engine, _panorama, 1, 1, panoramaPixels, {panoramaRegion});

    std::vector<float> cubePixels;
    std::vector<VkBufferImageCopy> cubeRegions;
    for (uint32_t mip = 0; mip < _mipCount; ++mip) {
        const uint32_t size = CubeSize >> mip;
        const float roughness = float(mip) / float(_mipCount - 1);
        for (uint32_t face = 0; face < 6; ++face) {
            VkBufferImageCopy region{};
            region.bufferOffset = VkDeviceSize(cubePixels.size()) * sizeof(float);
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = mip;
            region.imageSubresource.baseArrayLayer = face;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {size, size, 1};
            cubeRegions.push_back(region);
            for (uint32_t y = 0; y < size; ++y) for (uint32_t x = 0; x < size; ++x) {
                const glm::vec3 n = cube_direction(face,
                    (2.f * (float(x) + 0.5f) / float(size)) - 1.f,
                    (2.f * (float(y) + 0.5f) / float(size)) - 1.f);
                glm::vec3 color(0.f);
                if (mip == 0) {
                    color = panorama.sample(n);
                } else {
                    float weight = 0.f;
                    for (uint32_t i = 0; i < 64; ++i) {
                        const glm::vec3 h = importance_sample_ggx(i, 64, roughness, n);
                        const glm::vec3 l = glm::normalize(2.f * glm::dot(n, h) * h - n);
                        const float noL = std::max(glm::dot(n, l), 0.f);
                        if (noL > 0.f) { color += panorama.sample(l) * noL; weight += noL; }
                    }
                    color /= std::max(weight, 1e-5f);
                }
                cubePixels.insert(cubePixels.end(), {color.r, color.g, color.b, 1.f});
            }
        }
    }
    create_image(_device, _allocator, CubeSize, CubeSize, _mipCount, 6,
        VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
        _cube, _cubeAllocation, _cubeView, VK_IMAGE_VIEW_TYPE_CUBE);
    upload(engine, _cube, _mipCount, 6, cubePixels, cubeRegions);

    std::vector<float> lutPixels;
    lutPixels.reserve(size_t(LutSize) * LutSize * 4);
    for (uint32_t y = 0; y < LutSize; ++y) for (uint32_t x = 0; x < LutSize; ++x) {
        const float roughness = (float(y) + 0.5f) / float(LutSize);
        const float noV = (float(x) + 0.5f) / float(LutSize);
        const glm::vec3 v(std::sqrt(std::max(1.f - noV * noV, 0.f)), 0.f, noV);
        const glm::vec3 n(0.f, 0.f, 1.f);
        float scale = 0.f, bias = 0.f;
        for (uint32_t i = 0; i < 128; ++i) {
            const glm::vec3 h = importance_sample_ggx(i, 128, roughness, n);
            const glm::vec3 l = glm::normalize(2.f * glm::dot(v, h) * h - v);
            const float noL = std::max(l.z, 0.f);
            const float noH = std::max(h.z, 0.f);
            const float voH = std::max(glm::dot(v, h), 0.f);
            if (noL <= 0.f || noH <= 0.f) continue;
            const float a = std::max(roughness * roughness, 0.0016f);
            const float g = smith_g1(noV, a) * smith_g1(noL, a);
            const float visible = g * voH / std::max(noH * noV, 1e-5f);
            const float fresnel = std::pow(1.f - voH, 5.f);
            scale += (1.f - fresnel) * visible;
            bias += fresnel * visible;
        }
        lutPixels.insert(lutPixels.end(), {scale / 128.f, bias / 128.f, 0.f, 1.f});
    }
    create_image(_device, _allocator, LutSize, LutSize, 1, 1,
        VK_FORMAT_R32G32B32A32_SFLOAT, 0, _brdfLut, _brdfAllocation,
        _brdfView, VK_IMAGE_VIEW_TYPE_2D);
    VkBufferImageCopy lutRegion{};
    lutRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    lutRegion.imageSubresource.layerCount = 1;
    lutRegion.imageExtent = {LutSize, LutSize, 1};
    upload(engine, _brdfLut, 1, 1, lutPixels, {lutRegion});

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = float(_mipCount - 1);
    VK_CHECK(vkCreateSampler(_device, &samplerInfo, nullptr, &_sampler));
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = 0.f;
    VK_CHECK(vkCreateSampler(_device, &samplerInfo, nullptr, &_panoramaSampler));
    _parameterBuffers.resize(FRAME_OVERLAP);
    _descriptorSets.resize(FRAME_OVERLAP);
    for (uint32_t frameIndex = 0; frameIndex < FRAME_OVERLAP; ++frameIndex) {
        _parameterBuffers[frameIndex] = engine.create_buffer(
            sizeof(GPUEnvironmentParameters), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU);
        _descriptorSets[frameIndex] =
            engine._descriptorSystem.allocate_persistent(DescriptorLayoutID::Environment);
        const VkDescriptorSet set = _descriptorSets[frameIndex];
        engine._descriptorSystem.write_image(set, 0, _cubeView, _sampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        engine._descriptorSystem.write_image(set, 1, _brdfView, _sampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        engine._descriptorSystem.write_image(set, 2, _panoramaView, _panoramaSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        engine._descriptorSystem.write_buffer(set, 3, _parameterBuffers[frameIndex].buffer,
            sizeof(GPUEnvironmentParameters), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        prepare_frame(frameIndex);
    }
}

void EnvironmentLighting::prepare_frame(uint32_t frameIndex) {
    const GPUEnvironmentParameters parameters{
        glm::vec4(std::clamp(intensity, 0.f, 2.f), 0.f, 0.f, 0.f)};
    AllocatedBuffer& buffer = _parameterBuffers.at(frameIndex);
    std::memcpy(buffer.info.pMappedData, &parameters, sizeof(parameters));
    VK_CHECK(vmaFlushAllocation(_allocator, buffer.allocation, 0, sizeof(parameters)));
}

void EnvironmentLighting::cleanup() {
    if (_device == VK_NULL_HANDLE) return;
    for (const AllocatedBuffer& buffer : _parameterBuffers) {
        vmaDestroyBuffer(_allocator, buffer.buffer, buffer.allocation);
    }
    _parameterBuffers.clear();
    _descriptorSets.clear();
    vkDestroySampler(_device, _panoramaSampler, nullptr);
    vkDestroySampler(_device, _sampler, nullptr);
    vkDestroyImageView(_device, _panoramaView, nullptr);
    vmaDestroyImage(_allocator, _panorama, _panoramaAllocation);
    vkDestroyImageView(_device, _brdfView, nullptr);
    vmaDestroyImage(_allocator, _brdfLut, _brdfAllocation);
    vkDestroyImageView(_device, _cubeView, nullptr);
    vmaDestroyImage(_allocator, _cube, _cubeAllocation);
    _device = VK_NULL_HANDLE;
}
