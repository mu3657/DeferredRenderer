#include <vk_types.h>
#include "vk_loader.h"

void MeshNode::Draw(const glm::mat4& topMatrix, DrawContext& ctx) {
    if (!visible || !mesh) return;
    glm::mat4 nodeMatrix = topMatrix * localTransform;

    RayTracingRenderInstance rayTracingInstance{};
    rayTracingInstance.mesh = mesh.get();
    rayTracingInstance.transform = nodeMatrix;
    rayTracingInstance.instanceID = static_cast<uint32_t>(ctx.RayTracingInstances.size());
    ctx.RayTracingInstances.push_back(rayTracingInstance);

    for (auto& s : mesh->surfaces) {
        RenderObject def;
        def.indexCount = s.count;
        def.firstIndex = s.startIndex;
        def.indexBuffer = mesh->meshBuffers.indexBuffer.buffer;
        def.indexBufferAddress = mesh->meshBuffers.indexBufferAddress;
        def.vertexCount = mesh->meshBuffers.vertexCount;
        def.material = &s.material->data;
        def.bounds = s.bounds;
        def.transform = nodeMatrix;
        def.vertexBufferAddress = mesh->meshBuffers.vertexBufferAddress;
        def.temporalID = temporalID;
        if (s.material->data.surface == MaterialSurface::Transparent) {
            ctx.TransparentSurfaces.push_back(def);
        } else {
            ctx.OpaqueSurfaces.push_back(def);
        }
    }

    // Node::Draw applies this node's local transform before recursing.
    Node::Draw(topMatrix, ctx);
}
