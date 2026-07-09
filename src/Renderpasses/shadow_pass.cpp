#include "Renderpasses/shadow_pass.h"

#include "vk_engine.h"
#include "vk_images.h"
#include "vk_initializers.h"
#include "vk_pipelines.h"

#include "imgui.h"
#include <Tracy/Tracy.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/norm.hpp>
#include <vector>

namespace {
glm::vec3 safe_normalize(glm::vec3 value, glm::vec3 fallback)
{
    if (glm::length2(value) < 0.000001f) {
        return fallback;
    }

    return glm::normalize(value);
}

std::array<glm::vec3, 8> make_camera_frustum_corners(
    const GPUSceneData& cameraSceneData,
    float nearDistance,
    float farDistance)
{
    const glm::mat4 invView = glm::inverse(cameraSceneData.view);
    const glm::vec3 cameraPosition = glm::vec3(invView[3]);
    const glm::vec3 cameraRight = safe_normalize(glm::vec3(invView[0]), glm::vec3(1.f, 0.f, 0.f));
    const glm::vec3 cameraUp = safe_normalize(glm::vec3(invView[1]), glm::vec3(0.f, 1.f, 0.f));
    const glm::vec3 cameraForward = safe_normalize(-glm::vec3(invView[2]), glm::vec3(0.f, 0.f, -1.f));

    const float tanHalfFovY = 1.f / std::abs(cameraSceneData.proj[1][1]);
    const float aspect = std::abs(cameraSceneData.proj[1][1] / cameraSceneData.proj[0][0]);

    auto make_corners_at_distance = [&](float distance, std::array<glm::vec3, 8>& corners, size_t baseIndex) {
        const float halfHeight = distance * tanHalfFovY;
        const float halfWidth = halfHeight * aspect;
        const glm::vec3 center = cameraPosition + cameraForward * distance;
        corners[baseIndex + 0] = center + cameraRight * halfWidth + cameraUp * halfHeight;
        corners[baseIndex + 1] = center - cameraRight * halfWidth + cameraUp * halfHeight;
        corners[baseIndex + 2] = center + cameraRight * halfWidth - cameraUp * halfHeight;
        corners[baseIndex + 3] = center - cameraRight * halfWidth - cameraUp * halfHeight;
    };

    std::array<glm::vec3, 8> corners{};
    make_corners_at_distance(nearDistance, corners, 0);
    make_corners_at_distance(farDistance, corners, 4);
    return corners;
}

GPUSceneData make_shadow_scene_data(
    const GPUSceneData& cameraSceneData,
    const GPULight& directionalLight,
    VkExtent2D shadowExtent,
    float cascadeNearDistance,
    float cascadeFarDistance,
    float depthPadding,
    float& outTexelWorldSize)
{
    cascadeNearDistance = std::max(cascadeNearDistance, 0.01f);
    cascadeFarDistance = std::max(cascadeFarDistance, cascadeNearDistance + 1.f);
    depthPadding = std::max(depthPadding, 1.f);

    const std::array<glm::vec3, 8> frustumCorners =
        make_camera_frustum_corners(cameraSceneData, cascadeNearDistance, cascadeFarDistance);

    glm::vec3 frustumCenter{0.f};
    for (const glm::vec3& corner : frustumCorners) {
        frustumCenter += corner;
    }
    frustumCenter /= static_cast<float>(frustumCorners.size());

    float frustumRadius = 0.f;
    for (const glm::vec3& corner : frustumCorners) {
        frustumRadius = std::max(frustumRadius, glm::length(corner - frustumCenter));
    }

    const glm::vec3 lightDir = safe_normalize(
        glm::vec3(directionalLight.directionType),
        glm::vec3(0.f, -1.f, 0.f));

    const glm::vec3 eye = frustumCenter - lightDir * (frustumRadius + depthPadding);
    const glm::vec3 up = std::abs(glm::dot(lightDir, glm::vec3(0.f, 1.f, 0.f))) > 0.95f
        ? glm::vec3(0.f, 0.f, 1.f)
        : glm::vec3(0.f, 1.f, 0.f);

    glm::mat4 view = glm::lookAt(eye, frustumCenter, up);

    glm::vec3 minBounds{std::numeric_limits<float>::max()};
    glm::vec3 maxBounds{-std::numeric_limits<float>::max()};
    for (const glm::vec3& corner : frustumCorners) {
        const glm::vec3 lightSpaceCorner = glm::vec3(view * glm::vec4(corner, 1.f));
        minBounds = glm::min(minBounds, lightSpaceCorner);
        maxBounds = glm::max(maxBounds, lightSpaceCorner);
    }

    const float width = maxBounds.x - minBounds.x;
    const float height = maxBounds.y - minBounds.y;
    const float halfExtent = std::max(width, height) * 0.5f;
    glm::vec2 lightSpaceCenter = (glm::vec2(minBounds) + glm::vec2(maxBounds)) * 0.5f;

    const float shadowTexelSize = (halfExtent * 2.f) / static_cast<float>(shadowExtent.width);
    outTexelWorldSize = shadowTexelSize;
    if (shadowTexelSize > 0.f) {
        lightSpaceCenter = glm::floor(lightSpaceCenter / shadowTexelSize) * shadowTexelSize;
    }

    const float left = lightSpaceCenter.x - halfExtent;
    const float right = lightSpaceCenter.x + halfExtent;
    const float bottom = lightSpaceCenter.y - halfExtent;
    const float top = lightSpaceCenter.y + halfExtent;

    const float nearDistance = std::max(0.01f, -maxBounds.z - depthPadding);
    const float farDistance = std::max(nearDistance + 1.f, -minBounds.z + depthPadding);

    // This renderer uses reverse-Z depth: clear depth to 0 and compare GREATER_OR_EQUAL.
    glm::mat4 proj = glm::ortho(left, right, bottom, top, farDistance, nearDistance);
    proj[1][1] *= -1.f;

    GPUSceneData shadowSceneData = cameraSceneData;
    shadowSceneData.view = view;
    shadowSceneData.proj = proj;
    shadowSceneData.viewproj = proj * view;
    return shadowSceneData;
}

std::array<float, SHADOW_CASCADE_COUNT> make_cascade_splits(
    float nearDistance,
    float farDistance,
    float splitLambda)
{
    nearDistance = std::max(nearDistance, 0.01f);
    farDistance = std::max(farDistance, nearDistance + 1.f);
    splitLambda = std::clamp(splitLambda, 0.f, 1.f);

    std::array<float, SHADOW_CASCADE_COUNT> splits{};
    for (uint32_t i = 0; i < SHADOW_CASCADE_COUNT; i++) {
        const float p = static_cast<float>(i + 1) / static_cast<float>(SHADOW_CASCADE_COUNT);
        const float logSplit = nearDistance * std::pow(farDistance / nearDistance, p);
        const float uniformSplit = nearDistance + (farDistance - nearDistance) * p;
        splits[i] = glm::mix(uniformSplit, logSplit, splitLambda);
    }

    splits[SHADOW_CASCADE_COUNT - 1] = farDistance;
    return splits;
}

VkRect2D cascade_atlas_rect(VkExtent2D atlasExtent, uint32_t cascadeIndex)
{
    const uint32_t cascadeWidth = atlasExtent.width / 2;
    const uint32_t cascadeHeight = atlasExtent.height / 2;
    return VkRect2D{
        VkOffset2D{
            static_cast<int32_t>((cascadeIndex % 2) * cascadeWidth),
            static_cast<int32_t>((cascadeIndex / 2) * cascadeHeight),
        },
        VkExtent2D{cascadeWidth, cascadeHeight},
    };
}
}


void ShadowPass::init(const RenderPassInitContext& ctx)
{
    _engine = &ctx.engine;
    _frames.resize(FRAME_OVERLAP);


    VkExtent3D shadowExtent = { _shadowExtent.width, _shadowExtent.height, 1 };
    _shadowDepthImage = _engine->create_image(
        shadowExtent,
        VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        false);

    VkSamplerCreateInfo sampl = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};

    sampl.magFilter = VK_FILTER_LINEAR;
    sampl.minFilter = VK_FILTER_LINEAR;
    sampl.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampl.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampl.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampl.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    vkCreateSampler(_engine->_device, &sampl, nullptr, &_shadowSampler);


    VkDescriptorSetLayout layouts[] = {
        _engine->_gpuSceneDataDescriptorLayout,
        _engine->_bindlessDescriptorLayout, // 第一版 opaque 可不用，但为了以后 masked 保持一致
    };

    VkPushConstantRange matrixRange{};
    matrixRange.offset = 0;
    matrixRange.size = sizeof(GPUDrawPushConstants);
    matrixRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkPipelineLayoutCreateInfo layoutCreateInfo = vkinit::pipeline_layout_create_info();
    layoutCreateInfo.setLayoutCount = 2;
    layoutCreateInfo.pSetLayouts = layouts;
    layoutCreateInfo.pPushConstantRanges = &matrixRange;
    layoutCreateInfo.pushConstantRangeCount = 1;


    VkPipelineLayout layout =
        _engine->pipelineRegistry.create_pipeline_layout(layoutCreateInfo);


    VkShaderModule meshVertexShader = VK_NULL_HANDLE;
    if (!vkutil::load_shader_module("../cmake-build-debug/shaders/shadow.vert.spv", _engine->_device, &meshVertexShader)
        && !vkutil::load_shader_module("../cmake-build-debug-mingw/shaders/shadow.vert.spv", _engine->_device, &meshVertexShader)) {
        fmt::println("Error when building the shadow vertex shader module");
        return;
    }
    //  TODO： 修改piplinebuilder 支持无frag build
    PipelineBuilder pipelineBuilder;
    pipelineBuilder.set_shaders(meshVertexShader, VK_NULL_HANDLE);
    pipelineBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    pipelineBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    pipelineBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    pipelineBuilder.set_multisampling_none();
    pipelineBuilder.disable_blending();
    pipelineBuilder.enable_depthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);
    pipelineBuilder.set_depth_format(_shadowDepthImage.imageFormat);
    pipelineBuilder._pipelineLayout = layout;

    _opaquePipeline = _engine->pipelineRegistry.create_material_pipeline(
        PipelineKey{
            RenderPassType::ShadowDepth,
            PipelineVariant::ShadowDepth_Opaque,
            ShadingModel::MetallicRoughness,
            MaterialSurface::Opaque,
        },
        pipelineBuilder);

    vkDestroyShaderModule(_engine->_device, meshVertexShader, nullptr);




    for (uint32_t i = 0; i < FRAME_OVERLAP; i++) {
        for (uint32_t cascadeIndex = 0; cascadeIndex < SHADOW_CASCADE_COUNT; cascadeIndex++) {
            _frames[i].sceneBuffers[cascadeIndex] = _engine->create_buffer(
                sizeof(GPUSceneData),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VMA_MEMORY_USAGE_CPU_TO_GPU);

            _frames[i].sceneDescriptors[cascadeIndex] =
                ctx.descriptors.allocate_frame(DescriptorLayoutID::FrameScene, i);

            ctx.descriptors.write_buffer(
                _frames[i].sceneDescriptors[cascadeIndex],
                0,
                _frames[i].sceneBuffers[cascadeIndex].buffer,
                sizeof(GPUSceneData),
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);

            ctx.descriptors.write_buffer(
                _frames[i].sceneDescriptors[cascadeIndex],
                1,
                _engine->_frames[i].objectStorageBuffer.buffer,
                static_cast<size_t>(_engine->_frames[i].objectStorageBuffer.info.size),
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        }

        _frames[i].shadowDataBuffer = _engine->create_buffer(
            sizeof(GPUShadowData),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU);

        _frames[i].descriptor =
            ctx.descriptors.allocate_frame(DescriptorLayoutID::ShadowInput, i);

        ctx.descriptors.write_image(
            _frames[i].descriptor,
            0,
            _shadowDepthImage.imageView,
            _shadowSampler,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

        ctx.descriptors.write_buffer(
            _frames[i].descriptor,
            1,
            _frames[i].shadowDataBuffer.buffer,
            sizeof(GPUShadowData),
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    }



}

void ShadowPass::cleanup()
{
    if (!_engine) {
        return;
    }

    for (ShadowFrameResources& frame : _frames) {
        for (AllocatedBuffer& sceneBuffer : frame.sceneBuffers) {
            if (sceneBuffer.buffer != VK_NULL_HANDLE) {
                _engine->destroy_buffer(sceneBuffer);
                sceneBuffer = {};
            }
        }
        if (frame.shadowDataBuffer.buffer != VK_NULL_HANDLE) {
            _engine->destroy_buffer(frame.shadowDataBuffer);
            frame.shadowDataBuffer = {};
        }
    }
    _frames.clear();

    if (_shadowSampler != VK_NULL_HANDLE) {
        vkDestroySampler(_engine->_device, _shadowSampler, nullptr);
        _shadowSampler = VK_NULL_HANDLE;
    }

    if (_shadowDepthImage.image != VK_NULL_HANDLE) {
        _engine->destroy_image(_shadowDepthImage);
        _shadowDepthImage = {};
    }

    _opaquePipeline = nullptr;
    _maskedPipeline = nullptr;
    _engine = nullptr;
}

void ShadowPass::execute(ShadowPassContext& ctx)
{
    ZoneScopedN("ShadowPass");

    VulkanEngine& engine = ctx.engine;
    VkCommandBuffer cmd = ctx.cmd;
    DrawContext& drawContext = ctx.drawContext;
    RenderPassStats& passStats = engine.stats.shadow;
    passStats = {};

    if (!_opaquePipeline || ctx.frameIndex >= _frames.size()) {
        TracyPlot("ShadowPass Draw Calls", static_cast<int64_t>(passStats.drawcall_count));
        TracyPlot("ShadowPass Triangles", static_cast<int64_t>(passStats.triangle_count));
        return;
    }

    ShadowFrameResources& shadowFrame = _frames[ctx.frameIndex];
    GPULight directionalLight = ctx.lightSystem.GetDirectionalLight();
    constexpr float cameraNearDistance = 0.1f;
    const std::array<float, SHADOW_CASCADE_COUNT> cascadeSplits =
        make_cascade_splits(cameraNearDistance, _maxDistance, _splitLambda);
    const VkExtent2D cascadeExtent{_shadowExtent.width / 2, _shadowExtent.height / 2};

    float cascadeNear = cameraNearDistance;
    for (uint32_t cascadeIndex = 0; cascadeIndex < SHADOW_CASCADE_COUNT; cascadeIndex++) {
        GPUSceneData shadowSceneData = make_shadow_scene_data(
            ctx.sceneData,
            directionalLight,
            cascadeExtent,
            cascadeNear,
            cascadeSplits[cascadeIndex],
            _depthPadding,
            _lastTexelWorldSize[cascadeIndex]);

        _shadowData.lightViewProj[cascadeIndex] = shadowSceneData.viewproj;
        _lastCascadeSplits[cascadeIndex] = cascadeSplits[cascadeIndex];
        std::memcpy(
            shadowFrame.sceneBuffers[cascadeIndex].info.pMappedData,
            &shadowSceneData,
            sizeof(GPUSceneData));

        cascadeNear = cascadeSplits[cascadeIndex];
    }

    _shadowData.cascadeSplits = glm::vec4(
        cascadeSplits[0],
        cascadeSplits[1],
        cascadeSplits[2],
        cascadeSplits[3]);
    _shadowData.lightDir = directionalLight.directionType;
    _shadowData.params = glm::vec4(
        _bias,
        _strength,
        1.0f / static_cast<float>(cascadeExtent.width),
        _enabled && directionalLight.colorIntensity.w > 0.f ? 1.0f : 0.0f);

    std::memcpy(shadowFrame.shadowDataBuffer.info.pMappedData, &_shadowData, sizeof(GPUShadowData));

    if (!_enabled || directionalLight.colorIntensity.w <= 0.f) {
        _lastVisibleCasters.fill(0);
        TracyPlot("ShadowPass Draw Calls", static_cast<int64_t>(passStats.drawcall_count));
        TracyPlot("ShadowPass Triangles", static_cast<int64_t>(passStats.triangle_count));
        return;
    }

    vkutil::transition_image(
        cmd,
        _shadowDepthImage.image,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
        _shadowDepthImage.imageView,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    VkRenderingInfo renderInfo{};
    renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderInfo.renderArea = VkRect2D{{0, 0}, _shadowExtent};
    renderInfo.layerCount = 1;
    renderInfo.colorAttachmentCount = 0;
    renderInfo.pColorAttachments = nullptr;
    renderInfo.pDepthAttachment = &depthAttachment;

    vkCmdBeginRendering(cmd, &renderInfo);

    cascadeNear = cameraNearDistance;
    for (uint32_t cascadeIndex = 0; cascadeIndex < SHADOW_CASCADE_COUNT; cascadeIndex++) {
        GPUSceneData shadowSceneData = make_shadow_scene_data(
            ctx.sceneData,
            directionalLight,
            cascadeExtent,
            cascadeNear,
            cascadeSplits[cascadeIndex],
            _depthPadding,
            _lastTexelWorldSize[cascadeIndex]);

        std::vector<uint32_t> opaqueDraws;
        opaqueDraws.reserve(drawContext.OpaqueSurfaces.size());

        for (uint32_t i = 0; i < drawContext.OpaqueSurfaces.size(); i++) {
            const RenderObject& renderObject = drawContext.OpaqueSurfaces[i];
            if (!renderObject.material || !renderObject.material->castsShadow) {
                continue;
            }
            if (renderObject.material->surface == MaterialSurface::Transparent) {
                continue;
            }
            if (util::is_visible(renderObject, shadowSceneData.viewproj)) {
                opaqueDraws.push_back(i);
            }
        }

        std::sort(opaqueDraws.begin(), opaqueDraws.end(), [&](uint32_t iA, uint32_t iB) {
            const RenderObject& A = drawContext.OpaqueSurfaces[iA];
            const RenderObject& B = drawContext.OpaqueSurfaces[iB];
            if (A.material == B.material) {
                return A.indexBuffer < B.indexBuffer;
            }

            return A.material < B.material;
        });
        _lastVisibleCasters[cascadeIndex] = static_cast<uint32_t>(opaqueDraws.size());

        VkRect2D cascadeRect = cascade_atlas_rect(_shadowExtent, cascadeIndex);
        VkViewport viewport{};
        viewport.x = static_cast<float>(cascadeRect.offset.x);
        viewport.y = static_cast<float>(cascadeRect.offset.y);
        viewport.width = static_cast<float>(cascadeRect.extent.width);
        viewport.height = static_cast<float>(cascadeRect.extent.height);
        viewport.minDepth = 0.f;
        viewport.maxDepth = 1.f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &cascadeRect);

        MaterialPipeline* lastPipeline = nullptr;
        VkBuffer lastIndexBuffer = VK_NULL_HANDLE;

        auto draw = [&](const RenderObject& renderObject) {
            MaterialPipeline* pipeline = engine.pipelineRegistry.get_material_pipeline(
                RenderPassType::ShadowDepth,
                *renderObject.material);

            if (!pipeline) {
                if (renderObject.material->surface != MaterialSurface::Opaque) {
                    return;
                }
                pipeline = _opaquePipeline;
            }

            if (!pipeline) {
                return;
            }

            if (pipeline != lastPipeline) {
                lastPipeline = pipeline;
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);

                VkDescriptorSet descriptorSets[] = {
                    shadowFrame.sceneDescriptors[cascadeIndex],
                    engine._bindlessDescriptorSet,
                };
                vkCmdBindDescriptorSets(
                    cmd,
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipeline->layout,
                    0,
                    2,
                    descriptorSets,
                    0,
                    nullptr);
            }

            if (renderObject.indexBuffer != lastIndexBuffer) {
                lastIndexBuffer = renderObject.indexBuffer;
                vkCmdBindIndexBuffer(cmd, renderObject.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            }

            GPUDrawPushConstants pushConstants{};
            pushConstants.worldMatrix = renderObject.transform;
            pushConstants.vertexBuffer = renderObject.vertexBufferAddress;
            pushConstants.materialID = renderObject.material->materialID;

            vkCmdPushConstants(
                cmd,
                pipeline->layout,
                VK_SHADER_STAGE_VERTEX_BIT,
                0,
                sizeof(GPUDrawPushConstants),
                &pushConstants);

            vkCmdDrawIndexed(cmd, renderObject.indexCount, 1, renderObject.firstIndex, 0, 0);
            passStats.drawcall_count++;
            passStats.triangle_count += renderObject.indexCount / 3;
        };

        for (uint32_t drawIndex : opaqueDraws) {
            draw(drawContext.OpaqueSurfaces[drawIndex]);
        }

        cascadeNear = cascadeSplits[cascadeIndex];
    }

    vkCmdEndRendering(cmd);

    vkutil::transition_image(
        cmd,
        _shadowDepthImage.image,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

    TracyPlot("ShadowPass Draw Calls", static_cast<int64_t>(passStats.drawcall_count));
    TracyPlot("ShadowPass Triangles", static_cast<int64_t>(passStats.triangle_count));
}

void ShadowPass::draw_debug_ui()
{
    if (!ImGui::Begin("Shadow Pass", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    ImGui::Checkbox("Enabled", &_enabled);
    ImGui::SliderFloat("Max distance", &_maxDistance, 10.f, 300.f, "%.1f");
    ImGui::SliderFloat("Split lambda", &_splitLambda, 0.f, 1.f, "%.2f");
    ImGui::SliderFloat("Depth padding", &_depthPadding, 1.f, 60.f, "%.1f");
    ImGui::SliderFloat("Bias", &_bias, 0.0005f, 0.03f, "%.5f");
    ImGui::SliderFloat("Strength", &_strength, 0.f, 1.f, "%.2f");
    ImGui::Text("Shadow atlas: %ux%u", _shadowExtent.width, _shadowExtent.height);
    for (uint32_t cascadeIndex = 0; cascadeIndex < SHADOW_CASCADE_COUNT; cascadeIndex++) {
        ImGui::Text(
            "Cascade %u split %.2f casters %u texel %.4f",
            cascadeIndex,
            _lastCascadeSplits[cascadeIndex],
            _lastVisibleCasters[cascadeIndex],
            _lastTexelWorldSize[cascadeIndex]);
    }

    ImGui::End();
}
