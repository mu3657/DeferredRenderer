#include "Renderpasses/geometry_pass.h"

#include "vk_engine.h"
#include "vk_initializers.h"

#include <Tracy/Tracy.hpp>

#include <algorithm>
#include <array>
#include <vector>

void GeometryPass::execute(GeometryPassContext& ctx)
{
    ZoneScopedN("GeometryPass");

    VulkanEngine& engine = ctx.engine;
    VkCommandBuffer cmd = ctx.cmd;
    DrawContext& drawContext = ctx.drawContext;
    RenderPassStats& passStats = engine.stats.geometry;
    passStats = {};

    std::vector<uint32_t> opaqueDraws;
    opaqueDraws.reserve(drawContext.OpaqueSurfaces.size());

    for (uint32_t i = 0; i < drawContext.OpaqueSurfaces.size(); i++) {
        if (util::is_visible(drawContext.OpaqueSurfaces[i], ctx.sceneData.viewproj)) {
            opaqueDraws.push_back(i);
        }
    }

    std::sort(opaqueDraws.begin(), opaqueDraws.end(), [&](const auto& iA, const auto& iB) {
        const RenderObject& A = drawContext.OpaqueSurfaces[iA];
        const RenderObject& B = drawContext.OpaqueSurfaces[iB];
        if (A.material == B.material) {
            return A.indexBuffer < B.indexBuffer;
        }

        return A.material < B.material;
    });

    VkClearValue clearColor;
    clearColor.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    VkRenderingAttachmentInfo albedoAttachment = vkinit::attachment_info(
        engine._gAlbedo.imageView, &clearColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo normalAttachment = vkinit::attachment_info(
        engine._gNormal.imageView, &clearColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ormAttachment = vkinit::attachment_info(
        engine._gORM.imageView, &clearColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
        engine._depthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo colorAttachments[] = {albedoAttachment, normalAttachment, ormAttachment};

    VkRenderingInfo renderInfo = {};
    renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderInfo.renderArea = VkRect2D{{0, 0}, ctx.drawExtent};
    renderInfo.layerCount = 1;
    renderInfo.colorAttachmentCount = 3;
    renderInfo.pColorAttachments = colorAttachments;
    renderInfo.pDepthAttachment = &depthAttachment;

    vkCmdBeginRendering(cmd, &renderInfo);

    VkViewport viewport = {};
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = ctx.drawExtent.width;
    viewport.height = ctx.drawExtent.height;
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {};
    scissor.offset.x = 0;
    scissor.offset.y = 0;
    scissor.extent.width = static_cast<uint32_t>(viewport.width);
    scissor.extent.height = static_cast<uint32_t>(viewport.height);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    GPUSceneData* sceneUniformData = static_cast<GPUSceneData*>(ctx.frame.cameraBuffer.info.pMappedData);
    *sceneUniformData = ctx.sceneData;

    VkDescriptorSet globalDescriptor = ctx.frame.globalDescriptor;
    MaterialPipeline* lastPipeline = nullptr;
    VkBuffer lastIndexBuffer = VK_NULL_HANDLE;

    auto draw = [&](const RenderObject& r) {
        MaterialPipeline* pipeline = engine.pipelineRegistry.get_material_pipeline(RenderPassType::GBuffer, *r.material);
        if (!pipeline && !r.material->technique) {
            pipeline = r.material->pipeline;
        }
        if (!pipeline) {
            return;
        }

        if (pipeline != lastPipeline) {
            lastPipeline = pipeline;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);

            VkDescriptorSet descriptorSets[] = {globalDescriptor, engine._bindlessDescriptorSet};
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

        if (r.indexBuffer != lastIndexBuffer) {
            lastIndexBuffer = r.indexBuffer;
            vkCmdBindIndexBuffer(cmd, r.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        }

        GPUDrawPushConstants pushConstants;
        pushConstants.worldMatrix = r.transform;
        pushConstants.vertexBuffer = r.vertexBufferAddress;
        pushConstants.materialID = r.material->materialID;

        vkCmdPushConstants(
            cmd,
            pipeline->layout,
            VK_SHADER_STAGE_VERTEX_BIT,
            0,
            sizeof(GPUDrawPushConstants),
            &pushConstants);

        passStats.drawcall_count++;
        passStats.triangle_count += r.indexCount / 3;
        vkCmdDrawIndexed(cmd, r.indexCount, 1, r.firstIndex, 0, 0);
    };

    for (auto& r : opaqueDraws) {
        draw(drawContext.OpaqueSurfaces[r]);
    }
    for (auto& r : drawContext.TransparentSurfaces) {
        draw(r);
    }

    drawContext.OpaqueSurfaces.clear();
    drawContext.TransparentSurfaces.clear();

    vkCmdEndRendering(cmd);

    TracyPlot("GeometryPass Draw Calls", static_cast<int64_t>(passStats.drawcall_count));
    TracyPlot("GeometryPass Triangles", static_cast<int64_t>(passStats.triangle_count));
}
