#include "Renderpasses/ContactShadow_pass.h"

#include "bendsss/bend_sss_cpu.h"
#include "light_system.h"
#include "vk_engine.h"
#include "vk_pipelines.h"

#include <Tracy/Tracy.hpp>

#include <algorithm>
#include <stdexcept>

#include "imgui.h"

namespace {
struct BendShadowPushConstants {
    glm::vec4 lightCoordinate;
    glm::ivec4 waveOffset;
    glm::vec4 tuning;
    glm::vec4 depthParams;
    glm::vec4 depthBounds;
    glm::uvec4 flags;
};

static_assert(sizeof(BendShadowPushConstants) == 96);

constexpr uint32_t FlagIgnoreEdgePixels = 1u << 0;
constexpr uint32_t FlagUsePrecisionOffset = 1u << 1;
constexpr uint32_t FlagBilinearSamplingOffset = 1u << 2;
constexpr uint32_t FlagDebugEdgeMask = 1u << 3;
constexpr uint32_t FlagDebugThreadIndex = 1u << 4;
constexpr uint32_t FlagDebugWaveIndex = 1u << 5;
}

void ContactShadowPass::init(const RenderPassInitContext& ctx)
{
    _engine = &ctx.engine;

    VkPhysicalDeviceSubgroupProperties subgroupProperties{};
    subgroupProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    VkPhysicalDeviceProperties2 deviceProperties{};
    deviceProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    deviceProperties.pNext = &subgroupProperties;
    vkGetPhysicalDeviceProperties2(ctx.engine._chosenGPU, &deviceProperties);

    const VkSubgroupFeatureFlags requiredSubgroupOperations =
        VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT;
    if ((subgroupProperties.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) == 0
        || (subgroupProperties.supportedOperations & requiredSubgroupOperations)
            != requiredSubgroupOperations) {
        throw std::runtime_error(
            "Bend SSS requires compute-shader subgroup basic and vote operations");
    }

    VkDescriptorSetLayout layout = ctx.descriptors.layout(DescriptorLayoutID::ContactShadowCompute);

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(BendShadowPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &layout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &_pipelineLayout));

    VkShaderModule shaderModule{VK_NULL_HANDLE};
    if (!vkutil::load_shader_module(
            "../cmake-build-debug/shaders/bend_sss.comp.hlsl.spv",
            ctx.device,
            &shaderModule)
        && !vkutil::load_shader_module(
            "../cmake-build-debug-mingw/shaders/bend_sss.comp.hlsl.spv",
            ctx.device,
            &shaderModule)) {
        throw std::runtime_error("Failed to load bend_sss.comp.hlsl.spv");
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

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    samplerInfo.minLod = 0.f;
    samplerInfo.maxLod = 0.f;
    VK_CHECK(vkCreateSampler(ctx.device, &samplerInfo, nullptr, &_pointBorderSampler));

    _computeDescriptorSet = ctx.descriptors.allocate_persistent(DescriptorLayoutID::ContactShadowCompute);
    ctx.descriptors.write_image(
        _computeDescriptorSet,
        0,
        ctx.engine._depthImage.imageView,
        VK_NULL_HANDLE,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    ctx.descriptors.write_image(
        _computeDescriptorSet,
        1,
        VK_NULL_HANDLE,
        _pointBorderSampler,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_DESCRIPTOR_TYPE_SAMPLER);
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
    vkDestroySampler(_engine->_device, _pointBorderSampler, nullptr);
    _pipeline = VK_NULL_HANDLE;
    _pipelineLayout = VK_NULL_HANDLE;
    _pointBorderSampler = VK_NULL_HANDLE;
    _computeDescriptorSet = VK_NULL_HANDLE;
    _lightingDescriptorSet = VK_NULL_HANDLE;
    _engine = nullptr;
}

void ContactShadowPass::execute(ContactShadowPassContext& ctx)
{
    ZoneScopedN("ContactShadowPass");

    const GPULight directionalLight = ctx.lightSystem.GetDirectionalLight();
    const glm::vec3 surfaceToLight = glm::normalize(-glm::vec3(directionalLight.directionType));


    glm::mat4 bendProjection = ctx.sceneData.proj;
    bendProjection[1][1] *= -1.f;
    const glm::vec4 lightProjection =
        bendProjection * ctx.sceneData.view * glm::vec4(surfaceToLight, 0.f);

    float lightProjectionArray[4] = {
        lightProjection.x,
        lightProjection.y,
        lightProjection.z,
        lightProjection.w,
    };
    int viewportSize[2] = {
        static_cast<int>(ctx.drawExtent.width),
        static_cast<int>(ctx.drawExtent.height),
    };
    int minRenderBounds[2] = {0, 0};
    int maxRenderBounds[2] = {viewportSize[0], viewportSize[1]};

    constexpr int BendWaveSize = 64;
    const Bend::DispatchList dispatchList = Bend::BuildDispatchList(
        lightProjectionArray,
        viewportSize,
        minRenderBounds,
        maxRenderBounds,
        false,
        BendWaveSize);

    uint32_t flags = 0;
    flags |= _ignoreEdgePixels ? FlagIgnoreEdgePixels : 0;
    flags |= _usePrecisionOffset ? FlagUsePrecisionOffset : 0;
    flags |= _bilinearSamplingOffsetMode ? FlagBilinearSamplingOffset : 0;
    flags |= _debugEdgeMask ? FlagDebugEdgeMask : 0;
    flags |= _debugThreadIndex ? FlagDebugThreadIndex : 0;
    flags |= _debugWaveIndex ? FlagDebugWaveIndex : 0;

    BendShadowPushConstants pushConstants{};
    pushConstants.lightCoordinate = glm::vec4(
        dispatchList.LightCoordinate_Shader[0],
        dispatchList.LightCoordinate_Shader[1],
        dispatchList.LightCoordinate_Shader[2],
        dispatchList.LightCoordinate_Shader[3]);
    pushConstants.tuning = glm::vec4(
        _surfaceThickness,
        _bilinearThreshold,
        _shadowContrast,
        _enabled ? _strength : 0.f);
    pushConstants.depthParams = glm::vec4(
        1.f / static_cast<float>(ctx.drawExtent.width),
        1.f / static_cast<float>(ctx.drawExtent.height),
        0.f,
        1.f);
    pushConstants.depthBounds = glm::vec4(0.f, 1.f, 0.f, 0.f);
    pushConstants.flags = glm::uvec4(flags, 0u, 0u, 0u);

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
    vkCmdBindDescriptorSets(
        ctx.cmd,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        _pipelineLayout,
        0,
        1,
        &_computeDescriptorSet,
        0,
        nullptr);

    for (int dispatchIndex = 0; dispatchIndex < dispatchList.DispatchCount; dispatchIndex++) {
        const Bend::DispatchData& dispatch = dispatchList.Dispatch[dispatchIndex];
        pushConstants.waveOffset = glm::ivec4(
            dispatch.WaveOffset_Shader[0],
            dispatch.WaveOffset_Shader[1],
            0,
            0);
        vkCmdPushConstants(
            ctx.cmd,
            _pipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT,
            0,
            sizeof(BendShadowPushConstants),
            &pushConstants);
        vkCmdDispatch(
            ctx.cmd,
            static_cast<uint32_t>(dispatch.WaveCount[0]),
            static_cast<uint32_t>(dispatch.WaveCount[1]),
            static_cast<uint32_t>(dispatch.WaveCount[2]));
    }
}

void ContactShadowPass::draw_debug_ui()
{
    if (!ImGui::Begin("Contact Shadows", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    ImGui::Checkbox("Enabled", &_enabled);
    ImGui::SliderFloat("Surface thickness", &_surfaceThickness, 0.0001f, 0.05f, "%.4f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Bilinear threshold", &_bilinearThreshold, 0.001f, 0.1f, "%.4f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Shadow contrast", &_shadowContrast, 1.0f, 8.0f, "%.2f");
    ImGui::SliderFloat("Strength", &_strength, 0.0f, 1.0f, "%.2f");
    ImGui::Checkbox("Ignore edge pixels", &_ignoreEdgePixels);
    ImGui::Checkbox("Precision offset", &_usePrecisionOffset);
    ImGui::Checkbox("Bilinear offset mode", &_bilinearSamplingOffsetMode);
    ImGui::SeparatorText("Bend debug views");
    ImGui::Checkbox("Edge mask", &_debugEdgeMask);
    ImGui::Checkbox("Thread index", &_debugThreadIndex);
    ImGui::Checkbox("Wave index", &_debugWaveIndex);
    ImGui::TextUnformatted("Bend SSS: 60 samples, 64-thread wavefront projection.");
    ImGui::End();
}
