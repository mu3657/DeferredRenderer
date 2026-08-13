#include "Renderpasses/ContactShadow_pass.h"

#include "light_system.h"
#include "vk_engine.h"
#include "vk_pipelines.h"

#include <Tracy/Tracy.hpp>

#include <algorithm>
#include <stdexcept>

#include "imgui.h"

namespace {
struct ContactShadowPushConstants {
    glm::mat4 invViewProj;
    glm::vec4 lightDirectionMaxDistance;
    glm::vec4 traceParams;
    glm::vec4 marchParams;
};

static_assert(sizeof(ContactShadowPushConstants) == 112);
}

void ContactShadowPass::init(const RenderPassInitContext& ctx)
{
    _engine = &ctx.engine;

    VkDescriptorSetLayout layouts[] = {
        ctx.descriptors.layout(DescriptorLayoutID::FrameScene),
        ctx.descriptors.layout(DescriptorLayoutID::ContactShadowCompute),
    };

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(ContactShadowPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = layouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &_pipelineLayout));

    VkShaderModule shaderModule{VK_NULL_HANDLE};
    if (!vkutil::load_shader_module(
            "../cmake-build-debug/shaders/screen_space_shadow.comp.spv",
            ctx.device,
            &shaderModule)
        && !vkutil::load_shader_module(
            "../cmake-build-debug-mingw/shaders/screen_space_shadow.comp.spv",
            ctx.device,
            &shaderModule)) {
        throw std::runtime_error("Failed to load screen_space_shadow.comp.spv");
    }

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.layout = _pipelineLayout;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shaderModule;
    pipelineInfo.stage.pName = "main";
    VK_CHECK(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &_pipeline));
    vkDestroyShaderModule(ctx.device, shaderModule, nullptr);

    _computeDescriptorSet = ctx.descriptors.allocate_persistent(DescriptorLayoutID::ContactShadowCompute);
    ctx.descriptors.write_image(
        _computeDescriptorSet,
        0,
        ctx.engine._depthImage.imageView,
        ctx.engine._defaultSamplerNearest,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    ctx.descriptors.write_image(
        _computeDescriptorSet,
        1,
        ctx.engine._gNormal.imageView,
        ctx.engine._defaultSamplerNearest,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    ctx.descriptors.write_image(
        _computeDescriptorSet,
        2,
        ctx.engine._contactShadowImage.imageView,
        VK_NULL_HANDLE,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);

    _lightingDescriptorSet = ctx.descriptors.allocate_persistent(DescriptorLayoutID::ContactShadowInput);
    ctx.descriptors.write_image(
        _lightingDescriptorSet,
        0,
        ctx.engine._contactShadowImage.imageView,
        ctx.engine._defaultSamplerNearest,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
}

void ContactShadowPass::cleanup()
{
    if (!_engine) {
        return;
    }

    vkDestroyPipeline(_engine->_device, _pipeline, nullptr);
    vkDestroyPipelineLayout(_engine->_device, _pipelineLayout, nullptr);
    _pipeline = VK_NULL_HANDLE;
    _pipelineLayout = VK_NULL_HANDLE;
    _computeDescriptorSet = VK_NULL_HANDLE;
    _lightingDescriptorSet = VK_NULL_HANDLE;
    _engine = nullptr;
}

void ContactShadowPass::execute(ContactShadowPassContext& ctx)
{
    ZoneScopedN("ContactShadowPass");

    const GPULight directionalLight = ctx.lightSystem.GetDirectionalLight();
    const glm::vec3 surfaceToLight = glm::normalize(-glm::vec3(directionalLight.directionType));

    ContactShadowPushConstants pushConstants{};
    pushConstants.invViewProj = glm::inverse(ctx.sceneData.viewproj);
    pushConstants.lightDirectionMaxDistance = glm::vec4(surfaceToLight, _maxDistance);
    pushConstants.traceParams = glm::vec4(_thickness, _normalBias, _strength, _enabled ? 1.f : 0.f);
    pushConstants.marchParams = glm::vec4(
        static_cast<float>(_stepCount),
        _stepExponent,
        _edgeFade,
        0.f);

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
    VkDescriptorSet descriptorSets[] = {ctx.frame.globalDescriptor, _computeDescriptorSet};
    vkCmdBindDescriptorSets(
        ctx.cmd,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        _pipelineLayout,
        0,
        2,
        descriptorSets,
        0,
        nullptr);
    vkCmdPushConstants(
        ctx.cmd,
        _pipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(ContactShadowPushConstants),
        &pushConstants);

    constexpr uint32_t WorkgroupSize = 8;
    const uint32_t groupCountX = (ctx.drawExtent.width + WorkgroupSize - 1) / WorkgroupSize;
    const uint32_t groupCountY = (ctx.drawExtent.height + WorkgroupSize - 1) / WorkgroupSize;
    vkCmdDispatch(ctx.cmd, groupCountX, groupCountY, 1);
}

void ContactShadowPass::draw_debug_ui()
{
    if (!ImGui::Begin("Contact Shadows", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    ImGui::Checkbox("Enabled", &_enabled);
    ImGui::SliderInt("Steps", &_stepCount, 4, 64);
    ImGui::SliderFloat("Max distance", &_maxDistance, 0.05f, 5.0f, "%.2f");
    ImGui::SliderFloat("Thickness", &_thickness, 0.005f, 0.5f, "%.3f");
    ImGui::SliderFloat("Normal bias", &_normalBias, 0.0f, 0.2f, "%.3f");
    ImGui::SliderFloat("Strength", &_strength, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Edge fade", &_edgeFade, 0.0f, 0.25f, "%.3f");
    ImGui::SliderFloat("Step exponent", &_stepExponent, 0.5f, 3.0f, "%.2f");
    ImGui::TextUnformatted("Simple ray march placeholder; output is visibility (1 = lit).");
    ImGui::End();
}
