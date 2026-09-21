#include "Renderpasses/light_binning_pass.h"

#include "imgui.h"
#include "vk_engine.h"
#include "vk_pipelines.h"

#include <Tracy/Tracy.hpp>

#include <array>
#include <stdexcept>

namespace {
struct alignas(16) LightBinningPushConstants {
    glm::uvec4 grid{}; // xy = render extent, zw = tile counts
};

static_assert(sizeof(LightBinningPushConstants) == 16);
}

uint32_t LightBinningPass::tile_count_x(VkExtent2D extent) const
{
    return (extent.width + TileSize - 1u) / TileSize;
}

uint32_t LightBinningPass::tile_count_y(VkExtent2D extent) const
{
    return (extent.height + TileSize - 1u) / TileSize;
}

void LightBinningPass::init(const RenderPassInitContext& ctx)
{
    _engine = &ctx.engine;

    const std::array<VkDescriptorSetLayout, 2> layouts = {
        ctx.descriptors.layout(DescriptorLayoutID::FrameScene),
        ctx.descriptors.layout(DescriptorLayoutID::LightData),
    };

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.size = sizeof(LightBinningPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = static_cast<uint32_t>(layouts.size());
    layoutInfo.pSetLayouts = layouts.data();
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &_pipelineLayout));

    VkShaderModule shaderModule{VK_NULL_HANDLE};
    if (!vkutil::load_shader_module(
            "../cmake-build-debug/shaders/light_binning.comp.spv",
            ctx.device,
            &shaderModule)
        && !vkutil::load_shader_module(
            "../cmake-build-debug-mingw/shaders/light_binning.comp.spv",
            ctx.device,
            &shaderModule)
        && !vkutil::load_shader_module(
            "../cmake-build-release/shaders/light_binning.comp.spv",
            ctx.device,
            &shaderModule)) {
        throw std::runtime_error("Failed to load light_binning.comp.spv");
    }

    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.layout = _pipelineLayout;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shaderModule;
    pipelineInfo.stage.pName = "main";
    VK_CHECK(vkCreateComputePipelines(
        ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &_pipeline));
    vkDestroyShaderModule(ctx.device, shaderModule, nullptr);

    const VkExtent2D maximumExtent{
        ctx.engine._drawImage.imageExtent.width,
        ctx.engine._drawImage.imageExtent.height,
    };
    const VkDeviceSize maximumTileCount =
        static_cast<VkDeviceSize>(tile_count_x(maximumExtent))
        * static_cast<VkDeviceSize>(tile_count_y(maximumExtent));
    _maskBufferSize = maximumTileCount * LightMaskWordCount * sizeof(uint32_t);

    for (uint32_t frameIndex = 0; frameIndex < FRAME_OVERLAP; ++frameIndex) {
        FrameData& frame = ctx.engine._frames[frameIndex];
        frame.tileLightMaskBuffer = ctx.engine.create_buffer(
            _maskBufferSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY);
        ctx.descriptors.write_buffer(
            frame.lightDescriptor,
            2,
            frame.tileLightMaskBuffer.buffer,
            _maskBufferSize,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    }
}

void LightBinningPass::cleanup()
{
    if (!_engine) {
        return;
    }

    for (FrameData& frame : _engine->_frames) {
        if (frame.tileLightMaskBuffer.buffer != VK_NULL_HANDLE) {
            _engine->destroy_buffer(frame.tileLightMaskBuffer);
            frame.tileLightMaskBuffer = {};
        }
    }

    vkDestroyPipeline(_engine->_device, _pipeline, nullptr);
    vkDestroyPipelineLayout(_engine->_device, _pipelineLayout, nullptr);
    _pipeline = VK_NULL_HANDLE;
    _pipelineLayout = VK_NULL_HANDLE;
    _engine = nullptr;
}

void LightBinningPass::execute(RenderPassFrameContext& ctx)
{
    ZoneScopedN("LightBinningPass");

    _lastTileCountX = tile_count_x(ctx.drawExtent);
    _lastTileCountY = tile_count_y(ctx.drawExtent);
    if (!_enabled || _lastTileCountX == 0 || _lastTileCountY == 0) {
        return;
    }

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
    const std::array<VkDescriptorSet, 2> descriptorSets = {
        ctx.frame.globalDescriptor,
        ctx.frame.lightDescriptor,
    };
    vkCmdBindDescriptorSets(
        ctx.cmd,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        _pipelineLayout,
        0,
        static_cast<uint32_t>(descriptorSets.size()),
        descriptorSets.data(),
        0,
        nullptr);

    LightBinningPushConstants pushConstants{};
    pushConstants.grid = glm::uvec4(
        ctx.drawExtent.width,
        ctx.drawExtent.height,
        _lastTileCountX,
        _lastTileCountY);
    vkCmdPushConstants(
        ctx.cmd,
        _pipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(pushConstants),
        &pushConstants);
    vkCmdDispatch(ctx.cmd, _lastTileCountX, _lastTileCountY, 1);

    VkMemoryBarrier2 lightGridBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    lightGridBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    lightGridBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    lightGridBarrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    lightGridBarrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

    VkDependencyInfo dependencyInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependencyInfo.memoryBarrierCount = 1;
    dependencyInfo.pMemoryBarriers = &lightGridBarrier;
    vkCmdPipelineBarrier2(ctx.cmd, &dependencyInfo);
}

void LightBinningPass::draw_debug_ui()
{
    if (!ImGui::Begin("Tiled Deferred Lighting", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    ImGui::Checkbox("Enabled", &_enabled);
    ImGui::BeginDisabled(!_enabled);
    ImGui::Checkbox("Tile light-count heatmap", &_debugHeatmap);
    ImGui::EndDisabled();
    ImGui::Text("Tile size: %u x %u", TileSize, TileSize);
    ImGui::Text("Grid: %u x %u (%u tiles)",
        _lastTileCountX,
        _lastTileCountY,
        _lastTileCountX * _lastTileCountY);
    ImGui::Text("Mask storage: %.2f MiB / frame",
        static_cast<double>(_maskBufferSize) / (1024.0 * 1024.0));
    ImGui::TextDisabled("2D conservative binning; transparent lighting remains brute force.");
    ImGui::End();
}
