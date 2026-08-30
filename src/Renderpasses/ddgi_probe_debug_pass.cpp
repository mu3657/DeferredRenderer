#include "Renderpasses/ddgi_probe_debug_pass.h"

#include "GlobalilluminationStructure/ddgi_volume.h"
#include "vk_descriptor_system.h"
#include "vk_engine.h"
#include "vk_initializers.h"
#include "vk_pipelines.h"

#include <Tracy/Tracy.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>

#include "imgui.h"

namespace {

struct alignas(16) DDGIProbeDebugPushConstants {
    glm::vec4 originRadius{};
    glm::vec4 spacingIntensity{};
    glm::uvec4 countsAndMode{};
    glm::uvec4 debugParams{};
};

static_assert(sizeof(DDGIProbeDebugPushConstants) == 64);

struct SceneBounds {
    glm::vec3 minimum{};
    glm::vec3 maximum{};
};

uint32_t count_probe_centers_in_frustum(
    const DDGIVolume& volume,
    const glm::mat4& viewProjection)
{
    uint32_t visibleCount = 0;
    const glm::uvec3 counts = volume.desc().probeCounts;
    for (uint32_t z = 0; z < counts.z; ++z) {
        for (uint32_t y = 0; y < counts.y; ++y) {
            for (uint32_t x = 0; x < counts.x; ++x) {
                const glm::vec3 position = volume.desc().origin
                    + glm::vec3(x, y, z) * volume.desc().probeSpacing;
                const glm::vec4 clip = viewProjection * glm::vec4(position, 1.f);
                if (clip.w > 0.f
                    && std::abs(clip.x) <= clip.w
                    && std::abs(clip.y) <= clip.w
                    && clip.z >= 0.f
                    && clip.z <= clip.w) {
                    ++visibleCount;
                }
            }
        }
    }
    return visibleCount;
}

std::optional<SceneBounds> active_scene_bounds(const VulkanEngine& engine)
{
    const auto sceneIt = engine.loadedScenes.find(engine.activeSceneName);
    if (sceneIt == engine.loadedScenes.end() || !sceneIt->second) {
        return std::nullopt;
    }

    SceneBounds bounds{
        glm::vec3(std::numeric_limits<float>::max()),
        glm::vec3(std::numeric_limits<float>::lowest()),
    };
    bool foundBounds = false;
    const std::array<glm::vec3, 8> corners = {
        glm::vec3(-1.f, -1.f, -1.f),
        glm::vec3(1.f, -1.f, -1.f),
        glm::vec3(-1.f, 1.f, -1.f),
        glm::vec3(1.f, 1.f, -1.f),
        glm::vec3(-1.f, -1.f, 1.f),
        glm::vec3(1.f, -1.f, 1.f),
        glm::vec3(-1.f, 1.f, 1.f),
        glm::vec3(1.f, 1.f, 1.f),
    };

    for (const auto& [name, node] : sceneIt->second->nodes) {
        const std::shared_ptr<MeshNode> meshNode =
            std::dynamic_pointer_cast<MeshNode>(node);
        if (!meshNode || !meshNode->mesh) {
            continue;
        }
        for (const GeoSurface& surface : meshNode->mesh->surfaces) {
            for (const glm::vec3& corner : corners) {
                const glm::vec3 localPosition =
                    surface.bounds.origin + corner * surface.bounds.extents;
                const glm::vec3 worldPosition = glm::vec3(
                    meshNode->worldTransform * glm::vec4(localPosition, 1.f));
                bounds.minimum = glm::min(bounds.minimum, worldPosition);
                bounds.maximum = glm::max(bounds.maximum, worldPosition);
                foundBounds = true;
            }
        }
    }
    return foundBounds ? std::optional<SceneBounds>(bounds) : std::nullopt;
}

} // namespace

void DDGIProbeDebugPass::init(const RenderPassInitContext& ctx)
{
    _engine = &ctx.engine;

    const VkDescriptorSetLayout descriptorLayouts[] = {
        ctx.engine._gpuSceneDataDescriptorLayout,
        ctx.descriptors.layout(DescriptorLayoutID::GIInput),
    };
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.size = sizeof(DDGIProbeDebugPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = descriptorLayouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;
    VK_CHECK(vkCreatePipelineLayout(
        ctx.device, &layoutInfo, nullptr, &_pipelineLayout));

    VkShaderModule vertexShader{VK_NULL_HANDLE};
    VkShaderModule fragmentShader{VK_NULL_HANDLE};
    const bool vertexLoaded = vkutil::load_shader_module(
            "../cmake-build-debug/shaders/ddgi_probe_debug.vert.spv",
            ctx.device,
            &vertexShader)
        || vkutil::load_shader_module(
            "../cmake-build-debug-mingw/shaders/ddgi_probe_debug.vert.spv",
            ctx.device,
            &vertexShader);
    const bool fragmentLoaded = vkutil::load_shader_module(
            "../cmake-build-debug/shaders/ddgi_probe_debug.frag.spv",
            ctx.device,
            &fragmentShader)
        || vkutil::load_shader_module(
            "../cmake-build-debug-mingw/shaders/ddgi_probe_debug.frag.spv",
            ctx.device,
            &fragmentShader);
    if (!vertexLoaded || !fragmentLoaded) {
        if (vertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(ctx.device, vertexShader, nullptr);
        }
        if (fragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(ctx.device, fragmentShader, nullptr);
        }
        throw std::runtime_error("Failed to load DDGI probe debug shaders");
    }

    PipelineBuilder pipelineBuilder;
    pipelineBuilder.set_shaders(vertexShader, fragmentShader);
    pipelineBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    pipelineBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    pipelineBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    pipelineBuilder.set_multisampling_none();
    pipelineBuilder.disable_blending();
    pipelineBuilder.enable_depthtest(false, VK_COMPARE_OP_GREATER_OR_EQUAL);
    pipelineBuilder.set_color_attachment_format(ctx.engine._drawImage.imageFormat);
    pipelineBuilder.set_depth_format(ctx.engine._depthImage.imageFormat);
    pipelineBuilder._pipelineLayout = _pipelineLayout;
    _pipeline = pipelineBuilder.build_pipeline(ctx.device);
    pipelineBuilder.disable_depthtest();
    _xrayPipeline = pipelineBuilder.build_pipeline(ctx.device);

    vkDestroyShaderModule(ctx.device, vertexShader, nullptr);
    vkDestroyShaderModule(ctx.device, fragmentShader, nullptr);
}

void DDGIProbeDebugPass::cleanup()
{
    if (!_engine) {
        return;
    }

    vkDestroyPipeline(_engine->_device, _pipeline, nullptr);
    vkDestroyPipeline(_engine->_device, _xrayPipeline, nullptr);
    vkDestroyPipelineLayout(_engine->_device, _pipelineLayout, nullptr);
    _pipeline = VK_NULL_HANDLE;
    _xrayPipeline = VK_NULL_HANDLE;
    _pipelineLayout = VK_NULL_HANDLE;
    _lastDrawnProbeCount = 0;
    _engine = nullptr;
}

void DDGIProbeDebugPass::execute(DDGIProbeDebugPassContext& ctx)
{
    ZoneScopedN("DDGIProbeDebugPass");
    _lastDrawnProbeCount = 0;
    if (!_enabled || !ctx.volume.enabled()) {
        return;
    }

    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        ctx.targetImageView,
        nullptr,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAttachment.imageView = ctx.depthImageView;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo renderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderInfo.renderArea = VkRect2D{{0, 0}, ctx.drawExtent};
    renderInfo.layerCount = 1;
    renderInfo.colorAttachmentCount = 1;
    renderInfo.pColorAttachments = &colorAttachment;
    renderInfo.pDepthAttachment = &depthAttachment;
    vkCmdBeginRendering(ctx.cmd, &renderInfo);

    VkViewport viewport{};
    viewport.width = static_cast<float>(ctx.drawExtent.width);
    viewport.height = static_cast<float>(ctx.drawExtent.height);
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, ctx.drawExtent};
    vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);

    vkCmdBindPipeline(
        ctx.cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        _xray ? _xrayPipeline : _pipeline);
    const VkDescriptorSet descriptorSets[] = {
        ctx.frame.globalDescriptor,
        ctx.volume.sampling_descriptor_set(ctx.frameIndex),
    };
    vkCmdBindDescriptorSets(
        ctx.cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        _pipelineLayout,
        0,
        2,
        descriptorSets,
        0,
        nullptr);

    DDGIProbeDebugPushConstants pushConstants{};
    pushConstants.originRadius = glm::vec4(ctx.volume.desc().origin, _radius);
    pushConstants.spacingIntensity = glm::vec4(
        ctx.volume.desc().probeSpacing, _irradianceIntensity);
    const bool atlasesReady =
        ctx.volume.resources().irradiance.layout == VK_IMAGE_LAYOUT_GENERAL
        && ctx.volume.resources().distance.layout == VK_IMAGE_LAYOUT_GENERAL;
    const bool historyValid = atlasesReady
        && _engine->ddgiProbeBlendPass.history_valid(
            ctx.volume.history_clear_serial());
    pushConstants.countsAndMode = glm::uvec4(
        ctx.volume.desc().probeCounts,
        historyValid ? static_cast<uint32_t>(_mode) : 0u);
    pushConstants.debugParams.x = _screenSpaceTest
        ? 1u
        : (_cameraFrontTest ? 2u : 0u);
    vkCmdPushConstants(
        ctx.cmd,
        _pipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(pushConstants),
        &pushConstants);

    const DDGIProbeUpdateRange updateRange = ctx.volume.update_range();
    const bool singleMarkerTest = _screenSpaceTest || _cameraFrontTest;
    const uint32_t firstProbe = singleMarkerTest
        ? 0u
        : (_currentBatchOnly ? updateRange.firstProbe : 0u);
    const uint32_t probeCount = singleMarkerTest
        ? 1u
        : (_currentBatchOnly
            ? updateRange.probeCount
            : ctx.volume.total_probe_count());
    vkCmdDraw(ctx.cmd, 6u * probeCount, 1, 6u * firstProbe, 0);
    vkCmdEndRendering(ctx.cmd);

    _lastDrawnProbeCount = probeCount;
    TracyPlot("DDGI Debug Probes Drawn", static_cast<int64_t>(probeCount));
}

bool DDGIProbeDebugPass::fit_volume_to_active_scene(DDGIVolume& volume)
{
    if (!_engine) {
        return false;
    }
    const std::optional<SceneBounds> sceneBounds = active_scene_bounds(*_engine);
    if (!sceneBounds.has_value()) {
        return false;
    }

    constexpr float Margin = 0.5f;
    const glm::vec3 intervals = glm::max(
        glm::vec3(volume.desc().probeCounts - glm::uvec3(1u)),
        glm::vec3(1.f));
    const glm::vec3 fittedSpacing = glm::max(
        (sceneBounds->maximum - sceneBounds->minimum
            + glm::vec3(Margin * 2.f)) / intervals,
        glm::vec3(0.05f));
    volume.set_origin(sceneBounds->minimum - glm::vec3(Margin));
    volume.set_probe_spacing(fittedSpacing);
    return true;
}

void DDGIProbeDebugPass::draw_debug_ui()
{
    if (!ImGui::Begin("DDGI Probe Debug", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    ImGui::Checkbox("Visualize probes", &_enabled);
    ImGui::SameLine();
    ImGui::Checkbox("X-Ray", &_xray);
    if (ImGui::Checkbox("Screen-space visibility test", &_screenSpaceTest)
        && _screenSpaceTest) {
        _cameraFrontTest = false;
    }
    if (_screenSpaceTest) {
        ImGui::TextDisabled("Expected: one cyan marker at screen center");
    }
    if (ImGui::Checkbox("Camera-front world-space test", &_cameraFrontTest)
        && _cameraFrontTest) {
        _screenSpaceTest = false;
    }
    if (_cameraFrontTest) {
        ImGui::TextDisabled("Expected: one cyan marker 2 world units in front of camera");
    }
    ImGui::SliderFloat("Probe radius", &_radius, 0.02f, 0.5f, "%.3f");
    const char* modes[] = {
        "Position",
        "Irradiance",
        "Update state",
    };
    ImGui::Combo("Display mode", &_mode, modes, 3);
    if (_mode == 1) {
        ImGui::SliderFloat(
            "Irradiance intensity", &_irradianceIntensity, 0.f, 4.f);
    }
    ImGui::Checkbox("Current batch only", &_currentBatchOnly);
    ImGui::Text("Last submitted probes: %u", _lastDrawnProbeCount);
    if (_enabled && _lastDrawnProbeCount == 0) {
        ImGui::TextDisabled("Waiting for the first atlas initialization");
    }

    if (_engine && _engine->ddgiVolume.initialized()) {
        DDGIVolume& volume = _engine->ddgiVolume;
        glm::vec3 origin = volume.desc().origin;
        glm::vec3 spacing = volume.desc().probeSpacing;
        ImGui::Separator();
        ImGui::TextUnformatted("Volume placement");
        if (ImGui::DragFloat3("Origin", &origin.x, 0.05f)) {
            volume.set_origin(origin);
        }
        if (ImGui::DragFloat3("Spacing", &spacing.x, 0.02f, 0.05f, 100.f)) {
            spacing = glm::max(spacing, glm::vec3(0.05f));
            volume.set_probe_spacing(spacing);
        }

        const glm::vec3 probeMaximum = volume.desc().origin
            + glm::vec3(volume.desc().probeCounts - glm::uvec3(1u))
                * volume.desc().probeSpacing;
        ImGui::Text("Bounds min: %.2f, %.2f, %.2f",
            volume.desc().origin.x,
            volume.desc().origin.y,
            volume.desc().origin.z);
        ImGui::Text("Bounds max: %.2f, %.2f, %.2f",
            probeMaximum.x,
            probeMaximum.y,
            probeMaximum.z);

        const std::optional<SceneBounds> sceneBounds = active_scene_bounds(*_engine);
        if (sceneBounds.has_value()) {
            ImGui::Text("Scene min: %.2f, %.2f, %.2f",
                sceneBounds->minimum.x,
                sceneBounds->minimum.y,
                sceneBounds->minimum.z);
            ImGui::Text("Scene max: %.2f, %.2f, %.2f",
                sceneBounds->maximum.x,
                sceneBounds->maximum.y,
                sceneBounds->maximum.z);
        } else {
            ImGui::TextDisabled("Active scene bounds unavailable");
        }
        if (ImGui::Button("Fit active scene")) {
            fit_volume_to_active_scene(volume);
        }
        ImGui::SameLine();
        if (ImGui::Button("Center on camera")) {
            const glm::vec3 extent =
                glm::vec3(volume.desc().probeCounts - glm::uvec3(1u))
                * volume.desc().probeSpacing;
            volume.set_origin(_engine->mainCamera.position - extent * 0.5f);
        }
        if (ImGui::Button("Reset probe history")) {
            volume.request_history_reset();
        }
        if (!_engine->ddgiProbeBlendPass.history_valid(
                volume.history_clear_serial())) {
            ImGui::TextDisabled("Placement updated; waiting for the next probe blend");
        }

        ImGui::Text("Probe instances: %u", _currentBatchOnly
            ? volume.update_range().probeCount
            : volume.total_probe_count());
        ImGui::Text("CPU frustum probe centers: %u / %u",
            count_probe_centers_in_frustum(volume, _engine->sceneData.viewproj),
            volume.total_probe_count());
        ImGui::Text("History progress: %u / %u (%.1f%%)",
            volume.history_probe_count(),
            volume.total_probe_count(),
            100.f * static_cast<float>(volume.history_probe_count())
                / static_cast<float>(volume.total_probe_count()));
        ImGui::Text("Grid: %u x %u x %u",
            volume.desc().probeCounts.x,
            volume.desc().probeCounts.y,
            volume.desc().probeCounts.z);
    }
    ImGui::End();
}
