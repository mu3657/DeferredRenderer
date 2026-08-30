#include "Renderpasses/ddgi_probe_blend_pass.h"

#include "GlobalilluminationStructure/ddgi_volume.h"
#include "vk_descriptor_system.h"
#include "vk_engine.h"
#include "vk_pipelines.h"

#include <Tracy/Tracy.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>

#include "imgui.h"

namespace {

enum class DDGIProbeBlendStage : uint32_t {
    IrradianceInterior = 0,
    DistanceInterior = 1,
    BorderUpdate = 2,
};

struct alignas(16) DDGIProbeBlendPushConstants {
    glm::uvec4 irradianceLayout{};
    glm::uvec4 distanceLayout{};
    glm::uvec4 passParams{};
};

static_assert(sizeof(DDGIProbeBlendPushConstants) == 48);

VkImageMemoryBarrier2 make_atlas_transition(
    DDGIImageResource& resource,
    bool clear)
{
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    if (clear) {
        if (resource.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            barrier.srcAccessMask = VK_ACCESS_2_NONE;
        } else {
            barrier.srcStageMask =
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.srcAccessMask =
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        }
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    } else {
        barrier.srcStageMask =
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.srcAccessMask =
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT
            | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
            | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask =
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    }
    barrier.oldLayout = resource.layout;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.image = resource.image.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = resource.arrayLayers;
    resource.layout = VK_IMAGE_LAYOUT_GENERAL;
    return barrier;
}

void transition_atlases_for_blend(
    VkCommandBuffer cmd,
    DDGIImageResource& irradiance,
    DDGIImageResource& distance,
    bool clearHistory)
{
    const bool clearIrradiance = clearHistory
        || irradiance.layout == VK_IMAGE_LAYOUT_UNDEFINED;
    const bool clearDistance = clearHistory
        || distance.layout == VK_IMAGE_LAYOUT_UNDEFINED;
    const std::array<VkImageMemoryBarrier2, 2> transitions = {
        make_atlas_transition(irradiance, clearIrradiance),
        make_atlas_transition(distance, clearDistance),
    };
    VkDependencyInfo dependencyInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependencyInfo.imageMemoryBarrierCount = static_cast<uint32_t>(transitions.size());
    dependencyInfo.pImageMemoryBarriers = transitions.data();
    vkCmdPipelineBarrier2(cmd, &dependencyInfo);

    VkClearColorValue clearValue{};
    VkImageSubresourceRange clearRange{};
    clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clearRange.levelCount = 1;
    clearRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    if (clearIrradiance) {
        vkCmdClearColorImage(
            cmd,
            irradiance.image.image,
            VK_IMAGE_LAYOUT_GENERAL,
            &clearValue,
            1,
            &clearRange);
    }
    if (clearDistance) {
        vkCmdClearColorImage(
            cmd,
            distance.image.image,
            VK_IMAGE_LAYOUT_GENERAL,
            &clearValue,
            1,
            &clearRange);
    }

    std::array<VkImageMemoryBarrier2, 2> clearBarriers{};
    uint32_t clearBarrierCount = 0;
    const auto append_clear_barrier = [&](VkImage image) {
        VkImageMemoryBarrier2& barrier = clearBarriers[clearBarrierCount++];
        barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask =
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.image = image;
        barrier.subresourceRange = clearRange;
    };
    if (clearIrradiance) {
        append_clear_barrier(irradiance.image.image);
    }
    if (clearDistance) {
        append_clear_barrier(distance.image.image);
    }
    if (clearBarrierCount > 0) {
        VkDependencyInfo clearDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        clearDependency.imageMemoryBarrierCount = clearBarrierCount;
        clearDependency.pImageMemoryBarriers = clearBarriers.data();
        vkCmdPipelineBarrier2(cmd, &clearDependency);
    }
}

void make_atlas_interiors_visible_to_border(
    VkCommandBuffer cmd,
    VkImage irradiance,
    VkImage distance)
{
    std::array<VkImageMemoryBarrier2, 2> barriers{};
    for (VkImageMemoryBarrier2& barrier : barriers) {
        barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask =
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    }
    barriers[0].image = irradiance;
    barriers[1].image = distance;

    VkDependencyInfo dependencyInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependencyInfo.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
    dependencyInfo.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dependencyInfo);
}

void transition_probe_data_for_rtxgi(
    VkCommandBuffer cmd,
    DDGIImageResource& probeData,
    bool clear)
{
    VkImageMemoryBarrier2 transition{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    transition.srcStageMask = probeData.layout == VK_IMAGE_LAYOUT_UNDEFINED
        ? VK_PIPELINE_STAGE_2_NONE
        : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    transition.srcAccessMask = probeData.layout == VK_IMAGE_LAYOUT_UNDEFINED
        ? VK_ACCESS_2_NONE
        : (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    transition.dstStageMask = clear
        ? VK_PIPELINE_STAGE_2_TRANSFER_BIT
        : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    transition.dstAccessMask = clear
        ? VK_ACCESS_2_TRANSFER_WRITE_BIT
        : (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    transition.oldLayout = probeData.layout;
    transition.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    transition.image = probeData.image.image;
    transition.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    transition.subresourceRange.levelCount = 1;
    transition.subresourceRange.layerCount = probeData.arrayLayers;

    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &transition;
    vkCmdPipelineBarrier2(cmd, &dependency);
    probeData.layout = VK_IMAGE_LAYOUT_GENERAL;

    if (!clear) {
        return;
    }

    VkClearColorValue clearValue{};
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = probeData.arrayLayers;
    vkCmdClearColorImage(
        cmd,
        probeData.image.image,
        VK_IMAGE_LAYOUT_GENERAL,
        &clearValue,
        1,
        &range);

    VkImageMemoryBarrier2 clearBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    clearBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    clearBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    clearBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    clearBarrier.dstAccessMask =
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    clearBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    clearBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    clearBarrier.image = probeData.image.image;
    clearBarrier.subresourceRange = range;
    dependency.pImageMemoryBarriers = &clearBarrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void make_atlases_visible_to_sampling(
    VkCommandBuffer cmd,
    VkImage irradiance,
    VkImage distance)
{
    std::array<VkImageMemoryBarrier2, 2> barriers{};
    for (VkImageMemoryBarrier2& barrier : barriers) {
        barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.dstStageMask =
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask =
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    }
    barriers[0].image = irradiance;
    barriers[1].image = distance;

    VkDependencyInfo dependencyInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependencyInfo.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
    dependencyInfo.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dependencyInfo);
}

glm::uvec4 pack_layout(const DDGIAtlasLayout& layout)
{
    return glm::uvec4(
        layout.tilesPerRow,
        layout.tileTexels,
        layout.interiorTexels,
        0u);
}

} // namespace

void DDGIProbeBlendPass::init(const RenderPassInitContext& ctx)
{
    _engine = &ctx.engine;

    const VkDescriptorSetLayout descriptorLayout =
        ctx.descriptors.layout(DescriptorLayoutID::DDGIProbeBlend);
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.size = sizeof(DDGIProbeBlendPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;
    VK_CHECK(vkCreatePipelineLayout(
        ctx.device, &layoutInfo, nullptr, &_pipelineLayout));

    VkShaderModule shaderModule{VK_NULL_HANDLE};
    if (!vkutil::load_shader_module(
            "../cmake-build-debug/shaders/ddgi_probe_blend.comp.spv",
            ctx.device,
            &shaderModule)
        && !vkutil::load_shader_module(
            "../cmake-build-debug-mingw/shaders/ddgi_probe_blend.comp.spv",
            ctx.device,
            &shaderModule)) {
        throw std::runtime_error("Failed to load ddgi_probe_blend.comp.spv");
    }

    VkComputePipelineCreateInfo pipelineInfo{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.layout = _pipelineLayout;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shaderModule;
    pipelineInfo.stage.pName = "main";
    VK_CHECK(vkCreateComputePipelines(
        ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &_pipeline));
    vkDestroyShaderModule(ctx.device, shaderModule, nullptr);

    const VkDescriptorSetLayout rtxgiDescriptorLayout =
        ctx.descriptors.layout(DescriptorLayoutID::DDGIProbeBlendRTXGI);
    VkPushConstantRange rtxgiPushConstantRange{};
    rtxgiPushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    rtxgiPushConstantRange.size = rtxgi::DDGIRootConstants::GetSizeInBytes();

    layoutInfo.pSetLayouts = &rtxgiDescriptorLayout;
    layoutInfo.pPushConstantRanges = &rtxgiPushConstantRange;
    VK_CHECK(vkCreatePipelineLayout(
        ctx.device, &layoutInfo, nullptr, &_rtxgiPipelineLayout));

    const auto createRtxgiPipeline = [&](const char* path, VkPipeline& pipeline) {
        VkShaderModule module{VK_NULL_HANDLE};
        if (!vkutil::load_shader_module(path, ctx.device, &module)) {
            throw std::runtime_error(fmt::format("Failed to load {}", path));
        }
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.layout = _rtxgiPipelineLayout;
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = module;
        info.stage.pName = "DDGIProbeBlendingCS";
        VK_CHECK(vkCreateComputePipelines(
            ctx.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline));
        vkDestroyShaderModule(ctx.device, module, nullptr);
    };
    createRtxgiPipeline(
        "../cmake-build-debug/shaders/rtxgi_ddgi_irradiance_blend.comp.hlsl.spv",
        _rtxgiIrradiancePipeline);
    createRtxgiPipeline(
        "../cmake-build-debug/shaders/rtxgi_ddgi_distance_blend.comp.hlsl.spv",
        _rtxgiDistancePipeline);

    VkShaderModule diagnosticModule{VK_NULL_HANDLE};
    if (!vkutil::load_shader_module(
            "../cmake-build-debug/shaders/ddgi_probe_blend_diagnostic.comp.spv",
            ctx.device,
            &diagnosticModule)) {
        throw std::runtime_error(
            "Failed to load ddgi_probe_blend_diagnostic.comp.spv");
    }
    VkComputePipelineCreateInfo diagnosticInfo{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    diagnosticInfo.layout = _rtxgiPipelineLayout;
    diagnosticInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    diagnosticInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    diagnosticInfo.stage.module = diagnosticModule;
    diagnosticInfo.stage.pName = "main";
    VK_CHECK(vkCreateComputePipelines(
        ctx.device,
        VK_NULL_HANDLE,
        1,
        &diagnosticInfo,
        nullptr,
        &_rtxgiDiagnosticPipeline));
    vkDestroyShaderModule(ctx.device, diagnosticModule, nullptr);

    _pendingFrameStats.resize(ctx.engine.ddgiVolume.resources().frames.size());
}

void DDGIProbeBlendPass::cleanup()
{
    if (!_engine) {
        return;
    }

    vkDestroyPipeline(_engine->_device, _pipeline, nullptr);
    vkDestroyPipelineLayout(_engine->_device, _pipelineLayout, nullptr);
    vkDestroyPipeline(_engine->_device, _rtxgiIrradiancePipeline, nullptr);
    vkDestroyPipeline(_engine->_device, _rtxgiDistancePipeline, nullptr);
    vkDestroyPipeline(_engine->_device, _rtxgiDiagnosticPipeline, nullptr);
    vkDestroyPipelineLayout(_engine->_device, _rtxgiPipelineLayout, nullptr);
    _pipeline = VK_NULL_HANDLE;
    _pipelineLayout = VK_NULL_HANDLE;
    _rtxgiIrradiancePipeline = VK_NULL_HANDLE;
    _rtxgiDistancePipeline = VK_NULL_HANDLE;
    _rtxgiDiagnosticPipeline = VK_NULL_HANDLE;
    _rtxgiPipelineLayout = VK_NULL_HANDLE;
    _pendingFrameStats.clear();
    _stats = {};
    _lastClearedHistorySerial = 0;
    _hasHistory = false;
    _engine = nullptr;
}

void DDGIProbeBlendPass::execute(DDGIProbeBlendPassContext& ctx)
{
    ZoneScopedN("DDGIProbeBlendPass");
    _stats = {};

    if (!ctx.volume.enabled()) {
        return;
    }
    const DDGIProbeUpdateRange updateRange = ctx.volume.update_range();
    if (updateRange.probeCount == 0) {
        return;
    }

    DDGIVolumeResources& resources = ctx.volume.resources();
    const uint64_t historySerial = ctx.volume.history_clear_serial();
    const bool clearHistory = historySerial != _lastClearedHistorySerial;
    transition_atlases_for_blend(
        ctx.cmd, resources.irradiance, resources.distance, clearHistory);
    if (clearHistory) {
        _lastClearedHistorySerial = historySerial;
        _hasHistory = false;
    }

    if (_useOfficialRTXGI) {
        transition_probe_data_for_rtxgi(
            ctx.cmd, resources.probeData, clearHistory);
        const VkDescriptorSet descriptorSet =
            ctx.volume.rtxgi_update_descriptor_set(ctx.frameIndex);
        vkCmdBindDescriptorSets(
            ctx.cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            _rtxgiPipelineLayout,
            0,
            1,
            &descriptorSet,
            0,
            nullptr);

        // The renderer schedules probes in its legacy X,Y,Z linear order.
        // RTXGI dispatches X,Z,Y Texture2DArray workgroups. Split the current
        // range at X-row boundaries and pass the physical group offset through
        // RTXGI's otherwise unused reduction root constants. A plain dispatch
        // avoids relying on vkCmdDispatchBase semantics in DXC-generated HLSL.
        const glm::uvec3 counts = ctx.volume.desc().probeCounts;
        const auto dispatchBatch = [&]() {
            uint32_t probeIndex = updateRange.firstProbe;
            uint32_t remaining = updateRange.probeCount;
            while (remaining > 0) {
                const uint32_t x = probeIndex % counts.x;
                const uint32_t y = (probeIndex / counts.x) % counts.y;
                const uint32_t z = probeIndex / (counts.x * counts.y);
                const uint32_t segment = std::min(remaining, counts.x - x);
                rtxgi::DDGIRootConstants rootConstants{};
                rootConstants.volumeIndex = 0;
                rootConstants.reductionInputSizeX = x;
                rootConstants.reductionInputSizeY = z;
                rootConstants.reductionInputSizeZ = y;
                vkCmdPushConstants(
                    ctx.cmd,
                    _rtxgiPipelineLayout,
                    VK_SHADER_STAGE_COMPUTE_BIT,
                    0,
                    rtxgi::DDGIRootConstants::GetSizeInBytes(),
                    &rootConstants);
                vkCmdDispatch(ctx.cmd, segment, 1, 1);
                probeIndex += segment;
                remaining -= segment;
            }
        };

        vkCmdBindPipeline(
            ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _rtxgiIrradiancePipeline);
        dispatchBatch();
        vkCmdBindPipeline(
            ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _rtxgiDistancePipeline);
        dispatchBatch();

        // Read back aggregate values from exactly the atlas texels updated by
        // the official kernels. This pass never modifies DDGI textures.
        make_atlases_visible_to_sampling(
            ctx.cmd,
            resources.irradiance.image.image,
            resources.distance.image.image);
        const glm::uvec4 diagnosticConstants(
            updateRange.firstProbe,
            updateRange.probeCount,
            counts.x,
            counts.y);
        vkCmdBindPipeline(
            ctx.cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            _rtxgiDiagnosticPipeline);
        vkCmdPushConstants(
            ctx.cmd,
            _rtxgiPipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT,
            0,
            sizeof(diagnosticConstants),
            &diagnosticConstants);
        vkCmdDispatch(ctx.cmd, 1, 1, updateRange.probeCount);
    } else {
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
        const VkDescriptorSet descriptorSet =
            ctx.volume.update_descriptor_set(ctx.frameIndex);
        vkCmdBindDescriptorSets(
            ctx.cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            _pipelineLayout,
            0,
            1,
            &descriptorSet,
            0,
            nullptr);

        DDGIProbeBlendPushConstants pushConstants{};
        pushConstants.irradianceLayout = pack_layout(ctx.volume.irradiance_layout());
        pushConstants.distanceLayout = pack_layout(ctx.volume.distance_layout());

        const auto dispatchStage = [&](DDGIProbeBlendStage stage, uint32_t texelCount) {
            pushConstants.passParams.x = static_cast<uint32_t>(stage);
            vkCmdPushConstants(
                ctx.cmd,
                _pipelineLayout,
                VK_SHADER_STAGE_COMPUTE_BIT,
                0,
                sizeof(pushConstants),
                &pushConstants);
            vkCmdDispatch(
                ctx.cmd,
                (texelCount + 7u) / 8u,
                (texelCount + 7u) / 8u,
                updateRange.probeCount);
        };

        dispatchStage(
            DDGIProbeBlendStage::IrradianceInterior,
            ctx.volume.irradiance_layout().interiorTexels);
        dispatchStage(
            DDGIProbeBlendStage::DistanceInterior,
            ctx.volume.distance_layout().interiorTexels);

        make_atlas_interiors_visible_to_border(
            ctx.cmd,
            resources.irradiance.image.image,
            resources.distance.image.image);
        dispatchStage(
            DDGIProbeBlendStage::BorderUpdate,
            std::max(
                ctx.volume.irradiance_layout().tileTexels,
                ctx.volume.distance_layout().tileTexels));
    }
    make_atlases_visible_to_sampling(
        ctx.cmd,
        resources.irradiance.image.image,
        resources.distance.image.image);

    _stats.firstProbe = updateRange.firstProbe;
    _stats.probeCount = updateRange.probeCount;
    _stats.irradianceTexelCount = updateRange.probeCount
        * ctx.volume.irradiance_layout().interiorTexels
        * ctx.volume.irradiance_layout().interiorTexels;
    _stats.distanceTexelCount = updateRange.probeCount
        * ctx.volume.distance_layout().interiorTexels
        * ctx.volume.distance_layout().interiorTexels;
    if (ctx.frameIndex < _pendingFrameStats.size()) {
        _pendingFrameStats[ctx.frameIndex] = _stats;
    }
    _hasHistory = true;
    TracyPlot("DDGI Irradiance Texels Blended",
        static_cast<int64_t>(_stats.irradianceTexelCount));
    TracyPlot("DDGI Distance Texels Blended",
        static_cast<int64_t>(_stats.distanceTexelCount));
}

void DDGIProbeBlendPass::notify_frame_completed(uint32_t frameIndex)
{
    if (frameIndex >= _pendingFrameStats.size()) {
        return;
    }

    const DDGIProbeBlendDispatchStats completed = _pendingFrameStats[frameIndex];
    _pendingFrameStats[frameIndex] = {};
    if (completed.probeCount > 0) {
        fmt::println(
            "DDGI probe blend completed: probes [{}, {}), {} irradiance texels, {} distance texels, atlas borders ready",
            completed.firstProbe,
            completed.firstProbe + completed.probeCount,
            completed.irradianceTexelCount,
            completed.distanceTexelCount);
        std::fflush(stdout);
    }
}

void DDGIProbeBlendPass::draw_debug_ui()
{
    if (!ImGui::Begin("DDGI Probe Blend", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (_engine && _engine->ddgiVolume.initialized()) {
        DDGIVolume& volume = _engine->ddgiVolume;
        const DDGIAtlasLayout& irradiance = volume.irradiance_layout();
        const DDGIAtlasLayout& distance = volume.distance_layout();
        if (ImGui::Checkbox("Official RTXGI probe blending", &_useOfficialRTXGI)) {
            volume.request_history_reset();
        }
        ImGui::TextDisabled(
            _useOfficialRTXGI
                ? "NVIDIA ProbeBlendingCS.hlsl (explicit batched coordinate adapter)"
                : "DeferredRenderer legacy GLSL blending fallback");
        ImGui::TextDisabled(
            "RTXGI relocation/classification kernels compiled; activation awaits the 32 fixed-ray trace schedule");
        ImGui::Text("Hysteresis: %.3f", volume.desc().hysteresis);
        float backfaceThreshold = volume.desc().randomRayBackfaceThreshold;
        if (ImGui::SliderFloat(
                "Backface rejection ratio",
                &backfaceThreshold,
                0.01f,
                1.f,
                "%.2f")) {
            volume.set_random_ray_backface_threshold(backfaceThreshold);
        }
        ImGui::TextDisabled(
            "Reject irradiance update when the backface-hit ratio reaches this value");
        ImGui::Text("Irradiance array: %u x %u x %u (%u + border)",
            irradiance.extent.width,
            irradiance.extent.height,
            irradiance.arrayLayers,
            irradiance.interiorTexels);
        ImGui::Text("Distance array: %u x %u x %u (%u + border)",
            distance.extent.width,
            distance.extent.height,
            distance.arrayLayers,
            distance.interiorTexels);
        ImGui::Text("Last blend: %u probes", _stats.probeCount);
        ImGui::Text("Interior texels: %u irradiance, %u distance",
            _stats.irradianceTexelCount,
            _stats.distanceTexelCount);
        ImGui::Text("History progress: %u / %u (%.1f%%)",
            volume.history_probe_count(),
            volume.total_probe_count(),
            100.f * static_cast<float>(volume.history_probe_count())
                / static_cast<float>(volume.total_probe_count()));
        ImGui::Separator();
        ImGui::Checkbox("Deferred lighting", &_lightingEnabled);
        ImGui::SliderFloat("Indirect intensity", &_lightingIntensity, 0.f, 4.f);
        const char* debugModes[] = {
            "Composite",
            "Indirect only",
            "Coverage / confidence",
            "Probe irradiance heatmap",
            "Final contribution heatmap",
        };
        ImGui::Combo("Lighting output", &_lightingDebugMode, debugModes, 5);
        if (_lightingDebugMode == 2) {
            ImGui::TextDisabled("Magenta: outside volume; red: no history; green: valid history");
        } else if (_lightingDebugMode == 3 || _lightingDebugMode == 4) {
            ImGui::SliderFloat(
                "Heatmap exposure",
                &_heatmapExposure,
                1.f,
                256.f,
                "%.1f",
                ImGuiSliderFlags_Logarithmic);
            ImGui::TextDisabled("Black: zero; blue/cyan/yellow/red: increasing energy");
            if (_lightingDebugMode == 3) {
                ImGui::TextDisabled("Raw probe irradiance before receiver material and AO");
            } else {
                ImGui::TextDisabled("Final diffuse contribution after albedo, metallic, AO and 1/PI");
            }
        }
        if (!_hasHistory) {
            ImGui::TextDisabled("Waiting for the first completed probe blend");
        }
    }
    ImGui::End();
}
