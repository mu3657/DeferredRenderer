#include "Renderpasses/taa_pass.h"

#include "imgui.h"
#include "vk_engine.h"
#include "vk_images.h"
#include "vk_pipelines.h"

#include <Tracy/Tracy.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
struct alignas(16) PreviousTransformData {
    glm::mat4 worldMatrix{1.f};
    glm::uvec4 valid{};
};
static_assert(sizeof(PreviousTransformData) == 80);
struct TAAPushConstants {
    glm::uvec4 extentFlags{}; // width, height, history valid, debug mode
    glm::vec4 params{}; // history weight, relative depth tolerance, sky enabled
};
float halton(uint32_t index, uint32_t base)
{
    float result = 0.f, fraction = 1.f;
    while (index) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}
bool different_projection(const glm::mat4& a, const glm::mat4& b)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(a[c][r] - b[c][r]) > 1e-5f) return true;
    return false;
}
}

void TAAPass::init(const RenderPassInitContext& ctx)
{
    _engine = &ctx.engine;
    _transforms.resize(FRAME_OVERLAP);
    _sets.resize(FRAME_OVERLAP);
    for (auto& frameSets : _sets)
        for (auto& set : frameSets)
            set = ctx.descriptors.allocate_persistent(DescriptorLayoutID::TAA);

    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(ctx.device, &sampler, nullptr, &_sampler));

    const auto descriptorLayout = ctx.descriptors.layout(DescriptorLayoutID::TAA);
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TAAPushConstants)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &descriptorLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &range;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layout, nullptr, &_pipelineLayout));
    VkShaderModule shader{VK_NULL_HANDLE};
    if (!vkutil::load_shader_module("../cmake-build-debug/shaders/taa.comp.spv", ctx.device, &shader)
        && !vkutil::load_shader_module("../cmake-build-debug-mingw/shaders/taa.comp.spv", ctx.device, &shader))
        throw std::runtime_error("Failed to load taa.comp.spv");
    VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.layout = _pipelineLayout;
    pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module = shader;
    pipeline.stage.pName = "main";
    VK_CHECK(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &_pipeline));
    vkDestroyShaderModule(ctx.device, shader, nullptr);
    create_images();
}

void TAAPass::create_images()
{
    const auto size = _engine->_drawImage.imageExtent;
    _sourceImage = _engine->_drawImage.image;
    _sourceView = _engine->_drawImage.imageView;
    _depthView = _engine->_depthImage.imageView;
    _motion = _engine->create_image(size, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    for (auto& image : _history)
        image = _engine->create_image(size, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    const auto setName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetDeviceProcAddr(_engine->_device, "vkSetDebugUtilsObjectNameEXT"));
    if (setName) {
        const std::array<VkImage, 3> images{_motion.image, _history[0].image, _history[1].image};
        const char* names[] = {"TAA motion", "TAA history 0", "TAA history 1"};
        for (size_t i = 0; i < images.size(); ++i) {
            VkDebugUtilsObjectNameInfoEXT name{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
            name.objectType = VK_OBJECT_TYPE_IMAGE;
            name.objectHandle = reinterpret_cast<uint64_t>(images[i]);
            name.pObjectName = names[i];
            setName(_engine->_device, &name);
        }
    }
    _initializedImages = false;
    write_descriptors();
    reset_history();
}

void TAAPass::destroy_images()
{
    if (_motion.image) _engine->destroy_image(_motion);
    _motion = {};
    for (auto& image : _history) {
        if (image.image) _engine->destroy_image(image);
        image = {};
    }
}

void TAAPass::write_descriptors()
{
    auto& descriptors = _engine->_descriptorSystem;
    for (uint32_t frame = 0; frame < _sets.size(); ++frame) {
        for (uint32_t write = 0; write < 2; ++write) {
            const auto set = _sets[frame][write];
            descriptors.write_image(set, 0, _engine->_drawImage.imageView, VK_NULL_HANDLE,
                VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            descriptors.write_image(set, 1, _history[1 - write].imageView, _sampler,
                VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            descriptors.write_image(set, 2, _motion.imageView, _engine->_defaultSamplerNearest,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            descriptors.write_image(set, 3, _engine->_depthImage.imageView, _engine->_defaultSamplerNearest,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            descriptors.write_image(set, 4, _history[write].imageView, VK_NULL_HANDLE,
                VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            descriptors.write_buffer(set, 5, _engine->_frames[frame].cameraBuffer.buffer,
                sizeof(GPUSceneData), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        }
    }
}

void TAAPass::reset_history()
{
    _valid = false;
    _sampleIndex = 0;
    _previousObjects.clear();
}

void TAAPass::set_enabled(bool enabled)
{
    if (_enabled != enabled) {
        _enabled = enabled;
        reset_history();
    }
}

void TAAPass::prepare_frame(RenderPassFrameContext& ctx)
{
    ZoneScopedN("TAA Prepare");
    _resolved = false;
    _historyWasUsed = false;
    if (_sourceImage != _engine->_drawImage.image || _sourceView != _engine->_drawImage.imageView
        || _depthView != _engine->_depthImage.imageView
        || _motion.imageExtent.width != _engine->_drawImage.imageExtent.width
        || _motion.imageExtent.height != _engine->_drawImage.imageExtent.height) {
        // Rebuild descriptors only when all submissions using them have retired.
        VK_CHECK(vkDeviceWaitIdle(_engine->_device));
        destroy_images();
        create_images();
    }
    auto& scene = _engine->sceneData;
    const auto now = std::chrono::steady_clock::now();
    const glm::mat4 inverseView = glm::inverse(scene.view);
    const glm::mat4 previousInverseView = glm::inverse(_previousView);
    const bool cameraJump = glm::length(glm::vec3(inverseView[3] - previousInverseView[3])) > 10.f
        || glm::dot(glm::vec3(inverseView[2]), glm::vec3(previousInverseView[2])) < 0.70710678f;
    if (!_enabled || ctx.drawExtent.width != _extent.width || ctx.drawExtent.height != _extent.height
        || _sceneName != _engine->activeSceneName || _cameraCutSerial != _engine->mainCamera.cutSerial
        || _backgroundEffect != _engine->currentBackgroundEffect || _pbrDebugMode != _engine->pbrDebugMode
        || different_projection(scene.proj, _previousProjection) || cameraJump
        || (_valid && std::chrono::duration<float>(now - _lastSubmission).count() > 0.25f)) {
        reset_history();
    }
    _extent = ctx.drawExtent;
    scene.unjitteredViewproj = scene.viewproj;
    scene.previousViewproj = _valid ? _previousViewproj : scene.viewproj;
    glm::vec2 jitter{};
    if (_enabled && _extent.width && _extent.height) {
        const uint32_t sample = _sampleIndex % 8 + 1;
        jitter = glm::vec2(halton(sample, 2) - 0.5f, halton(sample, 3) - 0.5f)
            / glm::vec2(_extent.width, _extent.height);
        // clip.xy += 2 * UV jitter * clip.w (GLM is column-major).
        scene.proj[2][0] -= 2.f * jitter.x;
        scene.proj[2][1] -= 2.f * jitter.y;
        scene.viewproj = scene.proj * scene.view;
    }
    scene.jitter = glm::vec4(jitter, _valid ? _previousJitter : jitter);

    const auto& draws = _engine->mainDrawContext.OpaqueSurfaces;
    auto& transforms = _transforms[ctx.frameIndex];
    const size_t count = std::max(size_t(1), draws.size());
    if (transforms.capacity < count) {
        if (transforms.buffer.buffer) _engine->destroy_buffer(transforms.buffer);
        transforms.capacity = count;
        transforms.buffer = _engine->create_buffer(count * sizeof(PreviousTransformData),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU);
    }
    VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addressInfo.buffer = transforms.buffer.buffer;
    const auto address = vkGetBufferDeviceAddress(_engine->_device, &addressInfo);
    auto* data = static_cast<PreviousTransformData*>(transforms.buffer.info.pMappedData);
    _pendingObjects.clear();
    for (size_t i = 0; i < draws.size(); ++i) {
        auto& draw = _engine->mainDrawContext.OpaqueSurfaces[i];
        const auto previous = _previousObjects.find(draw.temporalID);
        const bool valid = _valid && draw.temporalID != 0 && previous != _previousObjects.end()
            && previous->second.vertices == draw.vertexBufferAddress;
        data[i].worldMatrix = valid ? previous->second.transform : draw.transform;
        data[i].valid = glm::uvec4(valid ? 1u : 0u, 0, 0, 0);
        draw.previousTransformAddress = address + i * sizeof(PreviousTransformData);
        _pendingObjects[draw.temporalID] = {draw.transform, draw.vertexBufferAddress};
    }
    VK_CHECK(vmaFlushAllocation(_engine->_allocator, transforms.buffer.allocation, 0, VK_WHOLE_SIZE));
}

void TAAPass::execute(RenderPassFrameContext& ctx)
{
    ZoneScopedN("TAAPass");
    if (!_enabled || !_extent.width || !_extent.height) return;
    _historyWasUsed = _valid;
    if (!_initializedImages) {
        for (const auto& image : _history)
            vkutil::transition_image(ctx.cmd, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        _initializedImages = true;
    }
    // GENERAL->GENERAL also orders last frame's history reads before overwriting.
    for (const auto& image : _history)
        vkutil::transition_image(ctx.cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    vkutil::transition_image(ctx.cmd, _engine->_drawImage.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
    const auto set = _sets[ctx.frameIndex][_writeIndex];
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayout, 0, 1, &set, 0, nullptr);
    TAAPushConstants constants{};
    constants.extentFlags = glm::uvec4(_extent.width, _extent.height, _valid ? 1u : 0u, 0u);
    constants.params = glm::vec4(_historyWeight, 0.02f, _engine->currentBackgroundEffect == 1 ? 1.f : 0.f, 0.f);
    vkCmdPushConstants(ctx.cmd, _pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
    vkCmdDispatch(ctx.cmd, (_extent.width + 7) / 8, (_extent.height + 7) / 8, 1);

    vkutil::transition_image(ctx.cmd, _history[_writeIndex].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkutil::transition_image(ctx.cmd, _engine->_drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};
    copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {_extent.width, _extent.height, 1};
    vkCmdCopyImage(ctx.cmd, _history[_writeIndex].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        _engine->_drawImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    vkutil::transition_image(ctx.cmd, _history[_writeIndex].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    vkutil::transition_image(ctx.cmd, _engine->_drawImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    _resolved = true;
}

void TAAPass::commit_frame()
{
    const auto& scene = _engine->sceneData;
    _previousView = scene.view;
    _previousProjection = scene.proj;
    _previousProjection[2][0] += 2.f * scene.jitter.x;
    _previousProjection[2][1] += 2.f * scene.jitter.y;
    _previousViewproj = scene.viewproj;
    _previousJitter = glm::vec2(scene.jitter);
    _previousObjects.swap(_pendingObjects);
    _sceneName = _engine->activeSceneName;
    _cameraCutSerial = _engine->mainCamera.cutSerial;
    _backgroundEffect = _engine->currentBackgroundEffect;
    _pbrDebugMode = _engine->pbrDebugMode;
    _lastSubmission = std::chrono::steady_clock::now();
    _valid = _resolved;
    if (_resolved) { _writeIndex = 1 - _writeIndex; ++_sampleIndex; }
}

void TAAPass::cleanup()
{
    if (!_engine) return;
    destroy_images();
    for (auto& transforms : _transforms)
        if (transforms.buffer.buffer) _engine->destroy_buffer(transforms.buffer);
    _transforms.clear();
    _sets.clear();
    vkDestroyPipeline(_engine->_device, _pipeline, nullptr);
    vkDestroyPipelineLayout(_engine->_device, _pipelineLayout, nullptr);
    vkDestroySampler(_engine->_device, _sampler, nullptr);
    _engine = nullptr;
}

void TAAPass::draw_debug_ui()
{
    bool enabled = _enabled;
    if (ImGui::Checkbox("Enabled", &enabled)) set_enabled(enabled);
    ImGui::SliderFloat("History weight", &_historyWeight, 0.f, 0.95f, "%.2f");
    if (ImGui::Button("Reset history")) reset_history();
    ImGui::Text("History: %s | Halton sample: %u", _valid ? "valid" : "reset", _sampleIndex % 8 + 1);
    ImGui::TextDisabled("HDR opaque + sky; transparent/debug overlays follow TAA");
}
