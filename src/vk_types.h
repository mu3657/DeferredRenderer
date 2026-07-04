// vulkan_guide.h : Include file for standard system include files,
// or project specific include files.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <span>
#include <array>
#include <functional>
#include <deque>

#include <vulkan/vulkan.h>
#include <vulkan/vk_enum_string_helper.h>
#include <vk_mem_alloc.h>

#include <fmt/core.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>



#define VK_CHECK(x)                                                     \
    do {                                                                \
        VkResult err = x;                                               \
        if (err) {                                                      \
            fmt::println("Detected Vulkan error: {}", string_VkResult(err)); \
            abort();                                                    \
        }                                                               \
    } while (0)

struct Bounds
{
    glm::vec3 origin;
    float sphereRadius;
    glm::vec3 extents;
};
struct MeshAsset;
struct AllocatedImage {
    VkImage image;
    VkImageView imageView;
    VmaAllocation allocation;
    VkExtent3D imageExtent;
    VkFormat imageFormat;
};
struct AllocatedBuffer {
    VkBuffer buffer;
    VmaAllocation allocation;
    VmaAllocationInfo info;
};

struct Vertex {

    glm::vec3 position;
    float uv_x;
    glm::vec3 normal;
    float uv_y;
    glm::vec4 color;
};

// holds the resources needed for a mesh
struct GPUMeshBuffers {

    AllocatedBuffer indexBuffer;
    AllocatedBuffer vertexBuffer;
    VkDeviceAddress vertexBufferAddress;
};

// push constants for our mesh object draws
struct GPUDrawPushConstants {
    glm::mat4 worldMatrix;
    VkDeviceAddress vertexBuffer;
    uint32_t materialID;
};

enum class MaterialSurface :uint8_t {
    MainColor,
    Transparent,
    Other
};

enum class PipelineVariant : uint8_t {
    GBuffer_MetallicRoughness,
    GBuffer_Unlit,
    ShadowDepth_Opaque,
    ShadowDepth_AlphaCutout,
    Lighting_Fullscreen,
};

struct MaterialPipeline {
	VkPipeline pipeline;
	VkPipelineLayout layout;
};

struct MaterialInstance {
    // Legacy fallback while pass-owned pipeline lookup is being introduced.
    MaterialPipeline* pipeline;
    uint32_t materialID;
    MaterialSurface passType;
    PipelineVariant gbufferVariant{PipelineVariant::GBuffer_MetallicRoughness};
};

struct DrawContext;

// base class for a renderable dynamic object
class IRenderable {

    virtual void Draw(const glm::mat4& topMatrix, DrawContext& ctx) = 0;
};

// implementation of a drawable scene node.
// the scene node can hold children and will also keep a transform to propagate
// to them
struct Node : public IRenderable {

    // parent pointer must be a weak pointer to avoid circular dependencies
    std::weak_ptr<Node> parent;
    std::vector<std::shared_ptr<Node>> children;

    std::string  name;           // display name (from GLTF node name)
    bool         visible { true };  // controlled by SceneOutliner

    glm::mat4 localTransform;
    glm::mat4 worldTransform;

    void refreshTransform(const glm::mat4& parentMatrix)
    {
        worldTransform = parentMatrix * localTransform;
        for (auto c : children) {
            c->refreshTransform(worldTransform);
        }
    }

    virtual void Draw(const glm::mat4& topMatrix, DrawContext& ctx)
    {
        if (!visible) return;   // outliner visibility gate
        // Accumulate this node's local transform for children
        glm::mat4 nodeMatrix = topMatrix * localTransform;
        for (auto& c : children) {
            c->Draw(nodeMatrix, ctx);
        }
    }
};

struct MeshNode : public Node {

    std::shared_ptr<MeshAsset> mesh;

    virtual void Draw(const glm::mat4& topMatrix, DrawContext& ctx) override;
};

// Light types matching NodeLight::type from the asset pipeline
enum class LightType : int {
    Point       = 0,
    Directional = 1,
    Spot        = 2,
};

struct GpuLight {
    glm::vec3  color;
    float      intensity;
    LightType  type;
    float      range;          // 0 = infinite (directional)
    glm::vec3  worldPosition;  // filled in during Draw() traversal
    float      _pad{};
};

struct LightNode : public Node {
    GpuLight light;

    virtual void Draw(const glm::mat4& topMatrix, DrawContext& ctx) override
    {
        // Store world-space position derived from the accumulated transform.
        // The light list is read directly from LoadedScene; DrawContext is not involved.
        light.worldPosition = glm::vec3(topMatrix * localTransform * glm::vec4(0.f, 0.f, 0.f, 1.f));
        Node::Draw(topMatrix, ctx); // recurse children
    }
};

struct RenderObject {
    uint32_t indexCount;
    uint32_t firstIndex;
    VkBuffer indexBuffer;

    MaterialInstance* material;
    Bounds bounds;
    glm::mat4 transform;
    VkDeviceAddress vertexBufferAddress;
};

struct DrawContext {
    std::vector<RenderObject> OpaqueSurfaces;
    std::vector<RenderObject> TransparentSurfaces;
};
