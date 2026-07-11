#include "Renderpasses/transparent_pass.h"

#include "vk_engine.h"
#include "vk_initializers.h"

#include <Tracy/Tracy.hpp>

#include <algorithm>
#include <vector>

namespace {
float camera_depth(const RenderObject& renderObject, const GPUSceneData& sceneData)
{
    const glm::vec3 worldCenter = glm::vec3(
        renderObject.transform * glm::vec4(renderObject.bounds.origin, 1.f));
    return -(sceneData.view * glm::vec4(worldCenter, 1.f)).z;
}
}

void TransparentPass::init(const RenderPassInitContext& ctx)
{
    _engine = &ctx.engine;
}

void TransparentPass::cleanup()
{
    _engine = nullptr;
}

void TransparentPass::execute(TransparentPassContext& ctx)
{
    ZoneScopedN("TransparentPass");

    VulkanEngine& engine = ctx.engine;
    DrawContext& drawContext = ctx.drawContext;
    RenderPassStats& passStats = engine.stats.transparent;
    passStats = {};

    std::vector<uint32_t> transparentDraws;
    transparentDraws.reserve(drawContext.TransparentSurfaces.size());
    for (uint32_t i = 0; i < drawContext.TransparentSurfaces.size(); i++) {
        const RenderObject& renderObject = drawContext.TransparentSurfaces[i];
        if (renderObject.material && util::is_visible(renderObject, ctx.sceneData.viewproj)) {
            transparentDraws.push_back(i);
        }
    }

    if (transparentDraws.empty()) {
        drawContext.TransparentSurfaces.clear();
        TracyPlot("TransparentPass Draw Calls", static_cast<int64_t>(0));
        TracyPlot("TransparentPass Triangles", static_cast<int64_t>(0));
        return;
    }

    std::stable_sort(transparentDraws.begin(), transparentDraws.end(), [&](uint32_t a, uint32_t b) {
        return camera_depth(drawContext.TransparentSurfaces[a], ctx.sceneData)
            > camera_depth(drawContext.TransparentSurfaces[b], ctx.sceneData);
    });

    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        ctx.targetImageView,
        nullptr,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = ctx.depthImageView;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo renderInfo{};
    renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderInfo.renderArea = VkRect2D{{0, 0}, ctx.drawExtent};
    renderInfo.layerCount = 1;
    renderInfo.colorAttachmentCount = 1;
    renderInfo.pColorAttachments = &colorAttachment;
    renderInfo.pDepthAttachment = &depthAttachment;

    VkCommandBuffer cmd = ctx.cmd;
    vkCmdBeginRendering(cmd, &renderInfo);

    VkViewport viewport{};
    viewport.width = static_cast<float>(ctx.drawExtent.width);
    viewport.height = static_cast<float>(ctx.drawExtent.height);
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{{0, 0}, ctx.drawExtent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    *static_cast<GPUSceneData*>(ctx.frame.cameraBuffer.info.pMappedData) = ctx.sceneData;
    ctx.lightSystem.upload_frame(ctx.frame);

    const VkDescriptorSet shadowSet = engine.shadowPass.descriptor_set(ctx.frameIndex);
    MaterialPipeline* lastPipeline = nullptr;
    VkBuffer lastIndexBuffer = VK_NULL_HANDLE;

    for (uint32_t drawIndex : transparentDraws) {
        const RenderObject& renderObject = drawContext.TransparentSurfaces[drawIndex];
        MaterialPipeline* pipeline = engine.pipelineRegistry.get_material_pipeline(
            RenderPassType::ForwardTransparent,
            *renderObject.material);
        if (!pipeline && !renderObject.material->technique) {
            pipeline = renderObject.material->pipeline;
        }
        if (!pipeline) {
            continue;
        }

        if (pipeline != lastPipeline) {
            lastPipeline = pipeline;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);

            VkDescriptorSet descriptorSets[] = {
                ctx.frame.globalDescriptor,
                engine._bindlessDescriptorSet,
                ctx.frame.lightDescriptor,
                shadowSet,
            };
            vkCmdBindDescriptorSets(
                cmd,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipeline->layout,
                0,
                4,
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
    }

    vkCmdEndRendering(cmd);
    drawContext.TransparentSurfaces.clear();

    TracyPlot("TransparentPass Draw Calls", static_cast<int64_t>(passStats.drawcall_count));
    TracyPlot("TransparentPass Triangles", static_cast<int64_t>(passStats.triangle_count));
}
