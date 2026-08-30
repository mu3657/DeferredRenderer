#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "../vk_types.h"

// Shader-visible flags stored in GPURayTracingGeometry::flags.
enum RayTracingGeometryFlagBits : uint32_t {
    RayTracingGeometryFlagNone = 0,
    RayTracingGeometryFlagAlphaTested = 1u << 0,
    RayTracingGeometryFlagDoubleSided = 1u << 1,
};

using RayTracingGeometryFlags = uint32_t;

//BLAS
// Metadata used to recover vertex attributes and material data after a ray-query hit.
// The current renderer uses tightly packed uint32 indices for every draw.
struct alignas(16) GPURayTracingGeometry {
    VkDeviceAddress vertexBufferAddress{0};
    VkDeviceAddress indexBufferAddress{0};

    uint32_t firstIndex{0};
    uint32_t indexCount{0};
    uint32_t vertexStride{0};
    uint32_t materialID{0};

    RayTracingGeometryFlags flags{RayTracingGeometryFlagNone};
    uint32_t padding0{0};
    uint32_t padding1{0};
    uint32_t padding2{0};
};

static_assert(sizeof(GPURayTracingGeometry) == 48);
static_assert(offsetof(GPURayTracingGeometry, vertexBufferAddress) == 0);
static_assert(offsetof(GPURayTracingGeometry, indexBufferAddress) == 8);
static_assert(offsetof(GPURayTracingGeometry, firstIndex) == 16);
static_assert(offsetof(GPURayTracingGeometry, flags) == 32);

//TLAS
// VkAccelerationStructureInstanceKHR::instanceCustomIndex addresses this table.
// geometryMetadataOffset + rayQueryGetIntersectionGeometryIndexEXT() then selects
// the matching GPURayTracingGeometry record.
struct alignas(16) GPURayTracingInstance {
    uint32_t geometryMetadataOffset{0};
    uint32_t instanceID{0};
    uint32_t flags{0};
    uint32_t padding{0};
};

static_assert(sizeof(GPURayTracingInstance) == 16);

// One BLAS geometry. Multiple entries may reference different index ranges in the
// same vertex and index buffers, matching MeshAsset::surfaces.
struct RayTracingGeometryDesc {
    VkBuffer vertexBuffer{VK_NULL_HANDLE};
    VkDeviceAddress vertexBufferAddress{0};
    uint32_t vertexCount{0};
    uint32_t vertexStride{sizeof(Vertex)};
    VkFormat vertexFormat{VK_FORMAT_R32G32B32_SFLOAT};

    VkBuffer indexBuffer{VK_NULL_HANDLE};
    VkDeviceAddress indexBufferAddress{0};
    uint32_t firstIndex{0};
    uint32_t indexCount{0};
    VkIndexType indexType{VK_INDEX_TYPE_UINT32};

    uint32_t materialID{0};
    RayTracingGeometryFlags shaderFlags{RayTracingGeometryFlagNone};
    VkGeometryFlagsKHR buildFlags{VK_GEOMETRY_OPAQUE_BIT_KHR};
};

// Registration input for one mesh BLAS. The caller owns sourceKey; RayTracingScene
// only uses it as a stable identity for deduplication and invalidation.
struct RayTracingMeshDesc {
    const void* sourceKey{nullptr};
    std::string debugName;
    std::vector<RayTracingGeometryDesc> geometries;
    VkBuildAccelerationStructureFlagsKHR buildFlags{
        VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR};
};

struct RayTracingInstanceDesc {
    uint32_t blasIndex{0};
    uint32_t instanceID{0};
    uint32_t mask{0xff};
    VkGeometryInstanceFlagsKHR flags{VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR};
    glm::mat4 transform{1.f};
};

struct RayTracingAccelerationStructure {
    VkAccelerationStructureKHR handle{VK_NULL_HANDLE};
    AllocatedBuffer storage{};
    VkDeviceAddress deviceAddress{0};
};

struct RayTracingBLAS {
    const void* sourceKey{nullptr};
    RayTracingAccelerationStructure accelerationStructure{};
    uint32_t geometryMetadataOffset{0};
    uint32_t geometryCount{0};
    VkBuildAccelerationStructureFlagsKHR buildFlags{0};
};

// TLAS resources are frame-local so transforms and instance buffers can be updated
// without touching resources still consumed by another in-flight frame.
struct RayTracingSceneFrameResources {
    RayTracingAccelerationStructure tlas{};
    AllocatedBuffer instanceBuffer{};
    AllocatedBuffer instanceMetadataBuffer{};
    AllocatedBuffer scratchBuffer{};
    uint32_t instanceCapacity{0};
    VkDeviceSize scratchCapacity{0};
    VkDeviceSize tlasStorageCapacity{0};
    uint64_t builtGeometryGeneration{0};
    uint64_t builtInstanceGeneration{0};
};

struct RayTracingSceneInitContext {
    VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VmaAllocator allocator{VK_NULL_HANDLE};
    uint32_t frameOverlap{0};
};

struct RayTracingSceneStats {
    uint32_t blasCount{0};
    uint32_t geometryCount{0};
    uint32_t instanceCount{0};
    uint32_t pendingBLASBuilds{0};
};

// Owns the acceleration structures and the shader-visible hit-geometry table.
// Lighting techniques such as DDGI consume this class but do not own it.
class RayTracingScene {
public:
    RayTracingScene() = default;
    RayTracingScene(const RayTracingScene&) = delete;
    RayTracingScene& operator=(const RayTracingScene&) = delete;

    void init(const RayTracingSceneInitContext& ctx);
    void cleanup();

    // Returns the stable BLAS index used by RayTracingInstanceDesc::blasIndex.
    uint32_t register_mesh(const RayTracingMeshDesc& mesh);
    void unregister_all_meshes();

    // Copies CPU instance descriptions. GPU upload and TLAS build/update are recorded
    // later by record_builds(), after the frame fence has made frameIndex writable.
    void set_instances(std::span<const RayTracingInstanceDesc> instances);
    void record_builds(VkCommandBuffer cmd, uint32_t frameIndex);

    void mark_static_geometry_dirty();
    void mark_instances_dirty();

    bool initialized() const { return _initialized; }
    bool ready(uint32_t frameIndex) const;

    VkAccelerationStructureKHR tlas(uint32_t frameIndex) const;
    const AllocatedBuffer& geometry_metadata_buffer() const { return _geometryMetadataBuffer; }
    const AllocatedBuffer& instance_metadata_buffer(uint32_t frameIndex) const;
    uint32_t geometry_count() const { return _stats.geometryCount; }
    const RayTracingSceneStats& stats() const { return _stats; }

private:
    VkPhysicalDevice _physicalDevice{VK_NULL_HANDLE};
    VkDevice _device{VK_NULL_HANDLE};
    VmaAllocator _allocator{VK_NULL_HANDLE};

    std::vector<RayTracingMeshDesc> _meshDescs;
    std::vector<RayTracingBLAS> _blases;
    std::vector<RayTracingInstanceDesc> _instances;
    std::vector<RayTracingSceneFrameResources> _frames;

    AllocatedBuffer _geometryMetadataBuffer{};
    RayTracingSceneStats _stats{};
    VkDeviceSize _scratchAlignment{256};
    uint64_t _geometryGeneration{1};
    uint64_t _instanceGeneration{1};
    bool _staticGeometryDirty{false};
    bool _instancesDirty{false};
    bool _initialized{false};
};
