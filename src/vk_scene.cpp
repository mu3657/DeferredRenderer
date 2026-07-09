#include "vk_scene.h"
#include "vk_engine.h"

void RenderScene::init(VulkanEngine* engine) {
    _engine = engine;
}

void RenderScene::cleanup() {
    clear();
}

uint32_t RenderScene::add_object(MeshAsset* mesh, MaterialInstance* material, const glm::mat4& transform) {
    uint32_t firstId = static_cast<uint32_t>(_objects.size());

    for (auto& surface : mesh->surfaces) {
        RenderObject obj;
        obj.indexCount = surface.count;
        obj.firstIndex = surface.startIndex;
        obj.indexBuffer = mesh->meshBuffers.indexBuffer.buffer;
        obj.vertexBufferAddress = mesh->meshBuffers.vertexBufferAddress;
        
        // If the surface has a material from glTF, wrap it. Else fallback to provided material.
        if (surface.material) {
            obj.material = &surface.material->data;
        } else {
            obj.material = material;
        }

        obj.transform = transform;
        _objects.push_back(obj);
    }

    return firstId;
}

RenderObject* RenderScene::get_object(uint32_t id) {
    if (id < _objects.size()) {
        return &_objects[id];
    }
    return nullptr;
}

void RenderScene::update_draw_context(DrawContext& ctx) {
    // Collect into context.
    // By default, if the material defines transparent vs opaque, we separate them by surface type.
    
    for (const auto& obj : _objects) {
        if (obj.material && obj.material->surface == MaterialSurface::Transparent) {
            ctx.TransparentSurfaces.push_back(obj);
        } else {
            ctx.OpaqueSurfaces.push_back(obj);
        }
    }
}

void RenderScene::clear() {
    _objects.clear();
}
