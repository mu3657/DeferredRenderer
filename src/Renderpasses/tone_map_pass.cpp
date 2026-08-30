#include "Renderpasses/tone_map_pass.h"

#include "imgui.h"
#include "vk_engine.h"
#include "vk_images.h"
#include "vk_pipelines.h"

#include <Tracy/Tracy.hpp>

#include <stdexcept>

namespace {
struct alignas(16) ToneMapPushConstants {
    glm::vec4 params{};
};

static_assert(sizeof(ToneMapPushConstants) == 16);
}

void ToneMapPass::init(const RenderPassInitContext& ctx)
{
    _engine = &ctx.engine;

    const VkDescriptorSetLayout descriptorLayout =
        ctx.descriptors.layout(DescriptorLayoutID::DrawImage);
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.size = sizeof(ToneMapPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;
    VK_CHECK(vkCreatePipelineLayout(
        ctx.device, &layoutInfo, nullptr, &_pipelineLayout));

    VkShaderModule shaderModule{VK_NULL_HANDLE};
    if (!vkutil::load_shader_module(
            "../cmake-build-debug/shaders/tonemap.comp.spv",
            ctx.device,
            &shaderModule)
        && !vkutil::load_shader_module(
            "../cmake-build-debug-mingw/shaders/tonemap.comp.spv",
            ctx.device,
            &shaderModule)) {
        throw std::runtime_error("Failed to load tonemap.comp.spv");
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
}

void ToneMapPass::cleanup()
{
    if (!_engine) {
        return;
    }
    vkDestroyPipeline(_engine->_device, _pipeline, nullptr);
    vkDestroyPipelineLayout(_engine->_device, _pipelineLayout, nullptr);
    _pipeline = VK_NULL_HANDLE;
    _pipelineLayout = VK_NULL_HANDLE;
    _engine = nullptr;
}

void ToneMapPass::execute(RenderPassFrameContext& ctx)
{
    ZoneScopedN("ToneMapPass");
    if (!_enabled || ctx.drawExtent.width == 0 || ctx.drawExtent.height == 0) {
        return;
    }

    vkutil::transition_image(
        ctx.cmd,
        ctx.engine._drawImage.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL);

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
    vkCmdBindDescriptorSets(
        ctx.cmd,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        _pipelineLayout,
        0,
        1,
        &ctx.engine._drawImageDescriptors,
        0,
        nullptr);

    ToneMapPushConstants constants{};
    constants.params.x = _exposure;
    vkCmdPushConstants(
        ctx.cmd,
        _pipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(constants),
        &constants);
    vkCmdDispatch(
        ctx.cmd,
        (ctx.drawExtent.width + 7u) / 8u,
        (ctx.drawExtent.height + 7u) / 8u,
        1);

    vkutil::transition_image(
        ctx.cmd,
        ctx.engine._drawImage.image,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

void ToneMapPass::draw_debug_ui()
{
    if (!ImGui::Begin("Tone Mapping", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::Checkbox("Enabled", &_enabled);
    ImGui::SliderFloat(
        "Exposure", &_exposure, 0.05f, 4.f, "%.2f", ImGuiSliderFlags_Logarithmic);
    ImGui::TextDisabled("ACES fitted, then linear to sRGB");
    ImGui::End();
}
