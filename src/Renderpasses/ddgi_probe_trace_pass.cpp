#include "Renderpasses/ddgi_probe_trace_pass.h"

#include "GlobalilluminationStructure/ddgi_volume.h"
#include "GlobalilluminationStructure/ray_tracing_scene.h"
#include "light_system.h"
#include "vk_descriptor_system.h"
#include "vk_engine.h"
#include "vk_pipelines.h"

#include <Tracy/Tracy.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "imgui.h"

namespace {

struct alignas(16) DDGIProbeTracePushConstants {
    uint32_t traceMode{0};
    uint32_t materialCount{0};
    uint32_t flags{0};
    float multiBounceStrength{0.f};
};

static_assert(sizeof(DDGIProbeTracePushConstants) == 16);

constexpr uint32_t DDGIProbeTraceFlagMultiBounce = 1u << 0;
constexpr uint32_t DDGIProbeTraceFlagSignedBackfaceDistance = 1u << 1;

glm::vec4 make_ray_rotation(uint32_t frameNumber)
{
    // A deterministic irrational-angle sequence avoids locking the same Fibonacci
    // ray directions to scene geometry while remaining reproducible in captures.
    constexpr float GoldenAngle = 2.39996322972865332f;
    const float angle = GoldenAngle * static_cast<float>(frameNumber + 1u);
    glm::vec3 axis(
        std::sin(angle * 0.73f),
        std::cos(angle * 1.17f),
        std::sin(angle * 1.91f + 0.37f));
    axis = glm::normalize(axis);
    const float halfAngle = angle * 0.5f;
    return glm::vec4(axis * std::sin(halfAngle), std::cos(halfAngle));
}

void transition_ray_data_for_trace(VkCommandBuffer cmd, DDGIImageResource& rayData)
{
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    if (rayData.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = VK_ACCESS_2_NONE;
    } else {
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask =
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    }
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    barrier.oldLayout = rayData.layout;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.image = rayData.image.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = rayData.arrayLayers;

    VkDependencyInfo dependencyInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    VkMemoryBarrier2 constantsBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    constantsBarrier.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    constantsBarrier.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
    constantsBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    constantsBarrier.dstAccessMask =
        VK_ACCESS_2_UNIFORM_READ_BIT
        | VK_ACCESS_2_SHADER_STORAGE_READ_BIT
        | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    dependencyInfo.memoryBarrierCount = 1;
    dependencyInfo.pMemoryBarriers = &constantsBarrier;
    dependencyInfo.imageMemoryBarrierCount = 1;
    dependencyInfo.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependencyInfo);
    rayData.layout = VK_IMAGE_LAYOUT_GENERAL;
}

void transition_probe_history_for_trace(
    VkCommandBuffer cmd,
    DDGIImageResource& irradiance,
    DDGIImageResource& distance)
{
    std::array<VkImageMemoryBarrier2, 2> barriers{};
    DDGIImageResource* resources[] = {&irradiance, &distance};
    for (size_t index = 0; index < barriers.size(); ++index) {
        DDGIImageResource& resource = *resources[index];
        VkImageMemoryBarrier2& barrier = barriers[index];
        barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        if (resource.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            barrier.srcAccessMask = VK_ACCESS_2_NONE;
        } else {
            barrier.srcStageMask =
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.srcAccessMask =
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                | VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        }
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = resource.layout;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.image = resource.image.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = resource.arrayLayers;
        resource.layout = VK_IMAGE_LAYOUT_GENERAL;
    }

    VkDependencyInfo dependencyInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependencyInfo.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
    dependencyInfo.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dependencyInfo);
}

void make_ray_data_visible_to_compute(
    VkCommandBuffer cmd,
    VkImage image,
    uint32_t arrayLayers)
{
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = arrayLayers;

    VkDependencyInfo dependencyInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependencyInfo.imageMemoryBarrierCount = 1;
    dependencyInfo.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependencyInfo);
}

} // namespace

void DDGIProbeTracePass::init(const RenderPassInitContext& ctx)
{
    _engine = &ctx.engine;

    const VkDescriptorSetLayout setLayouts[] = {
        ctx.descriptors.layout(DescriptorLayoutID::DDGIProbeTrace),
        ctx.engine._bindlessDescriptorLayout,
        ctx.descriptors.layout(DescriptorLayoutID::LightData),
    };

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 3;
    layoutInfo.pSetLayouts = setLayouts;
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.size = sizeof(DDGIProbeTracePushConstants);
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;
    VK_CHECK(vkCreatePipelineLayout(
        ctx.device, &layoutInfo, nullptr, &_pipelineLayout));

    VkShaderModule shaderModule{VK_NULL_HANDLE};
    if (!vkutil::load_shader_module(
            "../cmake-build-debug/shaders/ddgi_probe_trace.comp.spv",
            ctx.device,
            &shaderModule)
        && !vkutil::load_shader_module(
            "../cmake-build-debug-mingw/shaders/ddgi_probe_trace.comp.spv",
            ctx.device,
            &shaderModule)) {
        throw std::runtime_error("Failed to load ddgi_probe_trace.comp.spv");
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

    _pendingFrameStats.resize(ctx.engine.ddgiVolume.resources().frames.size());
}

void DDGIProbeTracePass::cleanup()
{
    if (!_engine) {
        return;
    }

    vkDestroyPipeline(_engine->_device, _pipeline, nullptr);
    vkDestroyPipelineLayout(_engine->_device, _pipelineLayout, nullptr);
    _pipeline = VK_NULL_HANDLE;
    _pipelineLayout = VK_NULL_HANDLE;
    _pendingFrameStats.clear();
    _stats = {};
    _dispatchInFlight = false;
    _engine = nullptr;
}

void DDGIProbeTracePass::execute(DDGIProbeTracePassContext& ctx)
{
    ZoneScopedN("DDGIProbeTracePass");
    _stats = {};

    if (!_enabled || _dispatchInFlight || !ctx.volume.enabled()
        || !ctx.rayTracingScene.ready(ctx.frameIndex)) {
        return;
    }

    ctx.volume.prepare_frame(
        ctx.frameIndex,
        static_cast<uint32_t>(ctx.engine._frameNumber),
        make_ray_rotation(static_cast<uint32_t>(ctx.engine._frameNumber)));
    const DDGIProbeUpdateRange updateRange = ctx.volume.update_range();
    if (updateRange.probeCount == 0) {
        return;
    }

    VkDescriptorSet traceDescriptor = ctx.volume.trace_descriptor_set(ctx.frameIndex);
    ctx.engine._descriptorSystem.write_acceleration_structure(
        traceDescriptor, 0, ctx.rayTracingScene.tlas(ctx.frameIndex));
    ctx.engine._descriptorSystem.write_buffer(
        traceDescriptor,
        3,
        ctx.rayTracingScene.geometry_metadata_buffer().buffer,
        ctx.rayTracingScene.geometry_count() * sizeof(GPURayTracingGeometry),
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    const AllocatedBuffer& instanceMetadata =
        ctx.rayTracingScene.instance_metadata_buffer(ctx.frameIndex);
    ctx.engine._descriptorSystem.write_buffer(
        traceDescriptor,
        4,
        instanceMetadata.buffer,
        instanceMetadata.info.size,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    ctx.lightSystem.upload_frame(ctx.frame);
    transition_probe_history_for_trace(
        ctx.cmd,
        ctx.volume.resources().irradiance,
        ctx.volume.resources().distance);
    transition_ray_data_for_trace(
        ctx.cmd, ctx.volume.resources().rayData);

    const VkDescriptorSet descriptorSets[] = {
        traceDescriptor,
        ctx.engine._bindlessDescriptorSet,
        ctx.frame.lightDescriptor,
    };
    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
    vkCmdBindDescriptorSets(
        ctx.cmd,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        _pipelineLayout,
        0,
        3,
        descriptorSets,
        0,
        nullptr);

    DDGIProbeTracePushConstants pushConstants{};
    pushConstants.traceMode = static_cast<uint32_t>(_traceMode);
    pushConstants.materialCount = ctx.engine.bindlessMaterialCount;
    // The first reset batch establishes valid zero-cleared atlases plus direct
    // lighting. Later batches may safely consume that partial history; invalid
    // neighbors carry zero distance confidence and therefore contribute nothing.
    const bool historyAvailable =
        ctx.volume.history_probe_count() > updateRange.probeCount;
    const bool useMultiBounce = _multiBounceEnabled
        && _traceMode == 3
        && historyAvailable;
    if (useMultiBounce) {
        pushConstants.flags |= DDGIProbeTraceFlagMultiBounce;
        pushConstants.multiBounceStrength = _multiBounceStrength;
    }
    if (_wholeProbeBackfaceRejection) {
        pushConstants.flags |= DDGIProbeTraceFlagSignedBackfaceDistance;
    }
    vkCmdPushConstants(
        ctx.cmd,
        _pipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(pushConstants),
        &pushConstants);

    const uint32_t raysPerProbe = ctx.volume.desc().raysPerProbe;
    vkCmdDispatch(
        ctx.cmd,
        (raysPerProbe + 7u) / 8u,
        (updateRange.probeCount + 7u) / 8u,
        1);
    make_ray_data_visible_to_compute(
        ctx.cmd,
        ctx.volume.resources().rayData.image.image,
        ctx.volume.resources().rayData.arrayLayers);

    _stats.firstProbe = updateRange.firstProbe;
    _stats.probeCount = updateRange.probeCount;
    _stats.rayCount = updateRange.probeCount * raysPerProbe;
    _stats.traceMode = pushConstants.traceMode;
    _stats.multiBounce = useMultiBounce ? 1u : 0u;
    if (ctx.frameIndex < _pendingFrameStats.size()) {
        _pendingFrameStats[ctx.frameIndex] = _stats;
    }
    _dispatchInFlight = true;
    TracyPlot("DDGI Probes Traced", static_cast<int64_t>(_stats.probeCount));
    TracyPlot("DDGI Rays Traced", static_cast<int64_t>(_stats.rayCount));
}

void DDGIProbeTracePass::notify_frame_completed(uint32_t frameIndex)
{
    if (frameIndex >= _pendingFrameStats.size()) {
        return;
    }

    const DDGIProbeTraceDispatchStats completed = _pendingFrameStats[frameIndex];
    _pendingFrameStats[frameIndex] = {};
    if (completed.rayCount > 0) {
        _dispatchInFlight = false;
    }
    if (completed.rayCount > 0) {
        fmt::println(
            "DDGI probe trace completed: mode {}, multi-bounce {}, probes [{}, {}), {} probes, {} primary rays, ray-data ready",
            completed.traceMode,
            completed.multiBounce != 0u ? "on" : "off",
            completed.firstProbe,
            completed.firstProbe + completed.probeCount,
            completed.probeCount,
            completed.rayCount);

        DDGIVolumeFrameResources& frame =
            _engine->ddgiVolume.resources().frames[frameIndex];
        VK_CHECK(vmaInvalidateAllocation(
            _engine->_allocator,
            frame.diagnosticsBuffer.allocation,
            0,
            sizeof(DDGIDiagnosticsGPU)));
        DDGIDiagnosticsGPU diagnostics{};
        std::memcpy(
            &diagnostics,
            frame.diagnosticsBuffer.info.pMappedData,
            sizeof(diagnostics));
        float maxRadiance = 0.f;
        float maxIrradiance = 0.f;
        std::memcpy(
            &maxRadiance,
            &diagnostics.maxRadianceBits,
            sizeof(maxRadiance));
        std::memcpy(
            &maxIrradiance,
            &diagnostics.maxIrradianceBits,
            sizeof(maxIrradiance));
        fmt::println(
            "DDGI GPU diagnostic: ray radiance nonzero {}/{}, max {:.6f}, hits front/back/miss {}/{}/{}, nonfinite {}; irradiance texels nonzero {}/{}, max {:.6f}, nonfinite {}",
            diagnostics.nonZeroRadianceRays,
            diagnostics.rayCount,
            maxRadiance,
            diagnostics.frontFaceHits,
            diagnostics.backFaceHits,
            diagnostics.missCount,
            diagnostics.nonFiniteRays,
            diagnostics.nonZeroIrradianceTexels,
            diagnostics.irradianceTexelCount,
            maxIrradiance,
            diagnostics.nonFiniteIrradianceTexels);
        std::fflush(stdout);
    }
}

void DDGIProbeTracePass::draw_debug_ui()
{
    if (!ImGui::Begin("DDGI Probe Trace", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    ImGui::Checkbox("Enabled", &_enabled);
    const char* traceModes[] = {
        "Distance Only",
        "Material Hit",
        "Direct Unshadowed",
        "Full Radiance",
        "Constant White (pipeline test)",
        "Hit Classification",
        "Base Color Factor",
        "Base Color Texture",
        "Emissive Texture",
        "Normal Texture",
        "Material 0 Factor",
    };
    if (ImGui::Combo("Trace mode", &_traceMode, traceModes, 11)
        && _engine && _engine->ddgiVolume.initialized()) {
        _engine->ddgiVolume.request_history_reset();
    }
    if (_traceMode == 4) {
        ImGui::TextDisabled("Expected result: bright probe irradiance everywhere");
    } else if (_traceMode == 5) {
        ImGui::TextDisabled("Green: front face; red: back face; blue: ray miss");
    } else if (_traceMode >= 6 && _traceMode <= 10) {
        ImGui::TextDisabled("Material source isolation; output is intentionally unlit");
    }
    bool multiBounceChanged = ImGui::Checkbox("Multi-bounce", &_multiBounceEnabled);
    ImGui::BeginDisabled(!_multiBounceEnabled);
    multiBounceChanged |= ImGui::SliderFloat(
        "Bounce strength", &_multiBounceStrength, 0.f, 1.f, "%.2f");
    ImGui::EndDisabled();
    if (multiBounceChanged
        && _engine
        && _engine->ddgiVolume.initialized()) {
        _engine->ddgiVolume.request_history_reset();
    }
    if (_traceMode != 3) {
        ImGui::TextDisabled("Multi-bounce is applied only in Full Radiance mode");
    } else if (_engine
        && _engine->ddgiVolume.initialized()
        && _engine->ddgiVolume.history_probe_count()
            <= _engine->ddgiVolume.update_range().probeCount) {
        ImGui::TextDisabled("Waiting for the first direct-light probe batch");
    } else {
        ImGui::Text("Last dispatch multi-bounce: %s", _stats.multiBounce != 0u ? "on" : "off");
    }
    if (ImGui::Checkbox(
            "RTXGI whole-probe backface rejection",
            &_wholeProbeBackfaceRejection)
        && _engine
        && _engine->ddgiVolume.initialized()) {
        _engine->ddgiVolume.request_history_reset();
    }
    ImGui::TextDisabled(
        _wholeProbeBackfaceRejection
            ? "Signed backface distances can reject an entire probe update"
            : "Backface radiance is discarded without rejecting the entire probe");
    if (_engine && _engine->ddgiVolume.initialized()) {
        const DDGIVolume& volume = _engine->ddgiVolume;
        const DDGIProbeUpdateRange range = volume.update_range();
        ImGui::Text("Probe grid: %u x %u x %u",
            volume.desc().probeCounts.x,
            volume.desc().probeCounts.y,
            volume.desc().probeCounts.z);
        ImGui::Text("Total probes: %u", volume.total_probe_count());
        ImGui::Text("Rays per probe: %u", volume.desc().raysPerProbe);
        ImGui::Text("Current update: %u .. %u",
            range.firstProbe,
            range.firstProbe + range.probeCount);
        ImGui::Text("Last dispatch: %u rays", _stats.rayCount);
        if (ImGui::Button("Reset probe history")) {
            _engine->ddgiVolume.request_history_reset();
        }
    }
    ImGui::End();
}
