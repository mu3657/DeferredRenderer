#pragma once

#include <unordered_map>
#include <string>
#include <memory>
#include <vulkan/vulkan.h>
#include <vk_types.h>
#include <vk_loader.h>

class VulkanEngine;

class AssetManager {
public:
    void init(VulkanEngine* engine);
    void cleanup();

    // Loads a mesh from a baked .mesh file
    MeshAsset* load_mesh(const std::string& path);

private:
    VulkanEngine* _engine{ nullptr };
    std::unordered_map<std::string, std::unique_ptr<MeshAsset>> _meshes;
};
