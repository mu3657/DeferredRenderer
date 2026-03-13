#pragma once

#include <vector>
#include <string>
#include <memory>
#include "vk_types.h"
#include "asset_manager.h" // For MeshAsset

class VulkanEngine;

// A simple scene that collects objects to be drawn
class RenderScene {
public:
    void init(VulkanEngine* engine);
    void cleanup();

    // Adds a mesh object to the scene and returns its unique ID (index)
    uint32_t add_object(MeshAsset* mesh, MaterialInstance* material, const glm::mat4& transform);
    
    // Retrieves a reference to an object for modifying its transform or material
    RenderObject* get_object(uint32_t id);

    // Updates the draw context with the currently active objects
    void update_draw_context(DrawContext& ctx);

    // Clears all objects
    void clear();

private:
    VulkanEngine* _engine{nullptr};

    std::vector<RenderObject> _objects;
};
