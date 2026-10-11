#pragma once

#include <unordered_map>
#include <string>
#include <memory>
#include <array>
#include <cstdint>
#include <vulkan/vulkan.h>
#include <vk_types.h>
#include <vk_loader.h>

class VulkanEngine;

enum class PrimitiveShape : uint8_t { Cube, Sphere, Plane };

class AssetManager {
public:
    void init(VulkanEngine* engine);
    void cleanup();

    // Loads a mesh from a baked .mesh file
    std::shared_ptr<MeshAsset> load_mesh(const std::string& path);
    std::shared_ptr<MeshAsset> get_primitive(PrimitiveShape shape);

    // Loads a full scene from a baked .pfb file
    std::shared_ptr<struct LoadedScene> load_prefab(const std::string& path);

private:
    std::shared_ptr<Material> load_material(const std::string& path);
    AllocatedImage load_texture(const std::string& path, bool srgb);

    VulkanEngine* _engine{ nullptr };
    std::unordered_map<std::string, std::shared_ptr<MeshAsset>> _meshes;
    std::unordered_map<std::string, std::shared_ptr<Material>> _materials;
    std::unordered_map<std::string, AllocatedImage> _textures;
    std::shared_ptr<Material> _defaultMaterial; // fallback when load_material fails
    std::array<std::shared_ptr<MeshAsset>, 3> _primitives{};
};
