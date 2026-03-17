#pragma once

#include <unordered_map>
#include <filesystem>

#include "vk_descriptors.h"
#include "vk_types.h"

struct Material {
    MaterialInstance data;
};

struct GeoSurface {
    uint32_t startIndex;
    uint32_t count;
    Bounds bounds;
    std::shared_ptr<Material> material;
};
struct MeshAsset {
    std::string name;

    std::vector<GeoSurface> surfaces;
    GPUMeshBuffers meshBuffers;


};

//forward declaration
class VulkanEngine;

struct LoadedScene : public IRenderable {

    // storage for all the data on a given scene file
    std::unordered_map<std::string, std::shared_ptr<MeshAsset>> meshes;
    std::unordered_map<std::string, std::shared_ptr<Node>> nodes;
    std::unordered_map<std::string, AllocatedImage> images;
    std::unordered_map<std::string, std::shared_ptr<Material>> materials;

    // nodes that dont have a parent, for iterating through the file in tree order
    std::vector<std::shared_ptr<Node>> topNodes;

    // all lights in the scene, populated by load_prefab; consumed by the lighting pass.
    // Weak references into the node graph — the nodes map owns the actual lifetime.
    std::vector<std::weak_ptr<LightNode>> lightNodes;

    std::vector<VkSampler> samplers;

    DescriptorAllocatorGrowable descriptorPool;

    AllocatedBuffer materialDataBuffer;

    VulkanEngine* creator;

    ~LoadedScene() { clearAll(); };

    virtual void Draw(const glm::mat4& topMatrix, DrawContext& ctx);

private:

    void clearAll();
};


std::optional<std::shared_ptr<LoadedScene>> loadScene(VulkanEngine* engine, std::string_view filePath);
