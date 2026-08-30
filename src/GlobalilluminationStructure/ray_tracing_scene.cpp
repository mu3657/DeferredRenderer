#include "GlobalilluminationStructure/ray_tracing_scene.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {

struct RayTracingFunctions {
    PFN_vkCreateAccelerationStructureKHR createAccelerationStructure{nullptr};
    PFN_vkDestroyAccelerationStructureKHR destroyAccelerationStructure{nullptr};
    PFN_vkGetAccelerationStructureBuildSizesKHR getBuildSizes{nullptr};
    PFN_vkCmdBuildAccelerationStructuresKHR cmdBuildAccelerationStructures{nullptr};
    PFN_vkGetAccelerationStructureDeviceAddressKHR getAccelerationStructureDeviceAddress{nullptr};
};

template <typename T>
T load_device_function(VkDevice device, const char* name)
{
    T function = reinterpret_cast<T>(vkGetDeviceProcAddr(device, name));
    if (function == nullptr) {
        throw std::runtime_error(std::string("Missing Vulkan device function: ") + name);
    }
    return function;
}

RayTracingFunctions load_ray_tracing_functions(VkDevice device)
{
    return RayTracingFunctions{
        load_device_function<PFN_vkCreateAccelerationStructureKHR>(
            device, "vkCreateAccelerationStructureKHR"),
        load_device_function<PFN_vkDestroyAccelerationStructureKHR>(
            device, "vkDestroyAccelerationStructureKHR"),
        load_device_function<PFN_vkGetAccelerationStructureBuildSizesKHR>(
            device, "vkGetAccelerationStructureBuildSizesKHR"),
        load_device_function<PFN_vkCmdBuildAccelerationStructuresKHR>(
            device, "vkCmdBuildAccelerationStructuresKHR"),
        load_device_function<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
            device, "vkGetAccelerationStructureDeviceAddressKHR"),
    };
}

VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

bool has_buffer(const AllocatedBuffer& buffer)
{
    return buffer.buffer != VK_NULL_HANDLE;
}

AllocatedBuffer create_buffer(
    VmaAllocator allocator,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VmaMemoryUsage memoryUsage,
    bool mapped)
{
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = memoryUsage;
    allocationInfo.flags = mapped ? VMA_ALLOCATION_CREATE_MAPPED_BIT : 0;

    AllocatedBuffer buffer{};
    VK_CHECK(vmaCreateBuffer(
        allocator,
        &bufferInfo,
        &allocationInfo,
        &buffer.buffer,
        &buffer.allocation,
        &buffer.info));
    return buffer;
}

void destroy_buffer(VmaAllocator allocator, AllocatedBuffer& buffer)
{
    if (!has_buffer(buffer)) {
        return;
    }
    vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
    buffer = {};
}

VkDeviceAddress get_buffer_address(VkDevice device, VkBuffer buffer)
{
    VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addressInfo.buffer = buffer;
    return vkGetBufferDeviceAddress(device, &addressInfo);
}

void ensure_buffer_capacity(
    VmaAllocator allocator,
    AllocatedBuffer& buffer,
    VkDeviceSize requiredSize,
    VkBufferUsageFlags usage,
    VmaMemoryUsage memoryUsage,
    bool mapped)
{
    if (has_buffer(buffer) && buffer.info.size >= requiredSize) {
        return;
    }

    destroy_buffer(allocator, buffer);
    buffer = create_buffer(allocator, requiredSize, usage, memoryUsage, mapped);
}

void destroy_acceleration_structure(
    const RayTracingFunctions& functions,
    VkDevice device,
    VmaAllocator allocator,
    RayTracingAccelerationStructure& accelerationStructure)
{
    if (accelerationStructure.handle != VK_NULL_HANDLE) {
        functions.destroyAccelerationStructure(
            device, accelerationStructure.handle, nullptr);
    }
    destroy_buffer(allocator, accelerationStructure.storage);
    accelerationStructure = {};
}

RayTracingAccelerationStructure create_acceleration_structure(
    const RayTracingFunctions& functions,
    VkDevice device,
    VmaAllocator allocator,
    VkAccelerationStructureTypeKHR type,
    VkDeviceSize size)
{
    RayTracingAccelerationStructure result{};
    result.storage = create_buffer(
        allocator,
        size,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY,
        false);

    VkAccelerationStructureCreateInfoKHR createInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    createInfo.buffer = result.storage.buffer;
    createInfo.size = size;
    createInfo.type = type;
    VK_CHECK(functions.createAccelerationStructure(
        device, &createInfo, nullptr, &result.handle));

    VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    addressInfo.accelerationStructure = result.handle;
    result.deviceAddress = functions.getAccelerationStructureDeviceAddress(device, &addressInfo);
    return result;
}

VkTransformMatrixKHR to_vk_transform(const glm::mat4& transform)
{
    VkTransformMatrixKHR result{};
    for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t column = 0; column < 4; ++column) {
            result.matrix[row][column] = transform[column][row];
        }
    }
    return result;
}

void cmd_memory_barrier(
    VkCommandBuffer cmd,
    VkPipelineStageFlags2 srcStage,
    VkAccessFlags2 srcAccess,
    VkPipelineStageFlags2 dstStage,
    VkAccessFlags2 dstAccess)
{
    VkMemoryBarrier2 memoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    memoryBarrier.srcStageMask = srcStage;
    memoryBarrier.srcAccessMask = srcAccess;
    memoryBarrier.dstStageMask = dstStage;
    memoryBarrier.dstAccessMask = dstAccess;

    VkDependencyInfo dependencyInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependencyInfo.memoryBarrierCount = 1;
    dependencyInfo.pMemoryBarriers = &memoryBarrier;
    vkCmdPipelineBarrier2(cmd, &dependencyInfo);
}

struct PendingBLASBuild {
    std::vector<VkAccelerationStructureGeometryKHR> geometries;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    std::vector<uint32_t> primitiveCounts;
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    VkAccelerationStructureBuildSizesInfoKHR sizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    VkDeviceSize scratchOffset{0};
};

} // namespace

void RayTracingScene::init(const RayTracingSceneInitContext& ctx)
{
    if (_initialized) {
        throw std::logic_error("RayTracingScene is already initialized");
    }
    if (ctx.physicalDevice == VK_NULL_HANDLE
        || ctx.device == VK_NULL_HANDLE
        || ctx.allocator == VK_NULL_HANDLE
        || ctx.frameOverlap == 0) {
        throw std::invalid_argument("RayTracingSceneInitContext is incomplete");
    }

    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructureFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    accelerationStructureFeatures.pNext = &rayQueryFeatures;
    VkPhysicalDeviceBufferDeviceAddressFeatures bufferDeviceAddressFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    bufferDeviceAddressFeatures.pNext = &accelerationStructureFeatures;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &bufferDeviceAddressFeatures;
    vkGetPhysicalDeviceFeatures2(ctx.physicalDevice, &features);

    if (bufferDeviceAddressFeatures.bufferDeviceAddress != VK_TRUE
        || accelerationStructureFeatures.accelerationStructure != VK_TRUE
        || rayQueryFeatures.rayQuery != VK_TRUE) {
        throw std::runtime_error(
            "RayTracingScene requires buffer device address, acceleration structures, and ray query");
    }

    // Fail during initialization instead of the first recorded build if the device
    // was created without the required extension entry points.
    (void)load_ray_tracing_functions(ctx.device);

    VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationStructureProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &accelerationStructureProperties;
    vkGetPhysicalDeviceProperties2(ctx.physicalDevice, &properties);

    _physicalDevice = ctx.physicalDevice;
    _device = ctx.device;
    _allocator = ctx.allocator;
    _scratchAlignment = std::max<VkDeviceSize>(
        accelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment,
        1);
    _frames.resize(ctx.frameOverlap);
    _staticGeometryDirty = false;
    _instancesDirty = false;
    _initialized = true;
}

void RayTracingScene::cleanup()
{
    if (!_initialized) {
        return;
    }

    VK_CHECK(vkDeviceWaitIdle(_device));
    const RayTracingFunctions functions = load_ray_tracing_functions(_device);

    for (RayTracingSceneFrameResources& frame : _frames) {
        destroy_acceleration_structure(
            functions, _device, _allocator, frame.tlas);
        destroy_buffer(_allocator, frame.instanceBuffer);
        destroy_buffer(_allocator, frame.instanceMetadataBuffer);
        destroy_buffer(_allocator, frame.scratchBuffer);
    }
    for (RayTracingBLAS& blas : _blases) {
        destroy_acceleration_structure(
            functions, _device, _allocator, blas.accelerationStructure);
    }
    destroy_buffer(_allocator, _geometryMetadataBuffer);

    _frames.clear();
    _instances.clear();
    _blases.clear();
    _meshDescs.clear();
    _stats = {};
    _physicalDevice = VK_NULL_HANDLE;
    _device = VK_NULL_HANDLE;
    _allocator = VK_NULL_HANDLE;
    _staticGeometryDirty = false;
    _instancesDirty = false;
    _initialized = false;
}

uint32_t RayTracingScene::register_mesh(const RayTracingMeshDesc& mesh)
{
    if (!_initialized) {
        throw std::logic_error("RayTracingScene must be initialized before registering meshes");
    }
    if (mesh.sourceKey == nullptr || mesh.geometries.empty()) {
        throw std::invalid_argument("RayTracingMeshDesc requires a source key and geometry");
    }

    for (const RayTracingGeometryDesc& geometry : mesh.geometries) {
        if (geometry.vertexBuffer == VK_NULL_HANDLE
            || geometry.vertexBufferAddress == 0
            || geometry.vertexCount == 0
            || geometry.vertexStride == 0
            || geometry.indexBuffer == VK_NULL_HANDLE
            || geometry.indexBufferAddress == 0
            || geometry.indexCount == 0
            || (geometry.indexCount % 3) != 0
            || geometry.indexType != VK_INDEX_TYPE_UINT32) {
            throw std::invalid_argument(
                "RayTracingGeometryDesc must contain uint32 indexed triangle geometry and device addresses");
        }
    }

    for (uint32_t index = 0; index < _meshDescs.size(); ++index) {
        if (_meshDescs[index].sourceKey == mesh.sourceKey) {
            _meshDescs[index] = mesh;
            mark_static_geometry_dirty();
            return index;
        }
    }

    const uint32_t index = static_cast<uint32_t>(_meshDescs.size());
    _meshDescs.push_back(mesh);
    _blases.emplace_back();
    _blases.back().sourceKey = mesh.sourceKey;
    mark_static_geometry_dirty();
    return index;
}

void RayTracingScene::unregister_all_meshes()
{
    if (!_initialized) {
        return;
    }

    VK_CHECK(vkDeviceWaitIdle(_device));
    const RayTracingFunctions functions = load_ray_tracing_functions(_device);
    for (RayTracingBLAS& blas : _blases) {
        destroy_acceleration_structure(
            functions, _device, _allocator, blas.accelerationStructure);
    }
    for (RayTracingSceneFrameResources& frame : _frames) {
        destroy_acceleration_structure(
            functions, _device, _allocator, frame.tlas);
        frame.builtGeometryGeneration = 0;
        frame.builtInstanceGeneration = 0;
    }
    destroy_buffer(_allocator, _geometryMetadataBuffer);

    _meshDescs.clear();
    _blases.clear();
    _instances.clear();
    _stats = {};
    ++_geometryGeneration;
    ++_instanceGeneration;
    _staticGeometryDirty = false;
    _instancesDirty = false;
}

void RayTracingScene::set_instances(std::span<const RayTracingInstanceDesc> instances)
{
    if (!_initialized) {
        throw std::logic_error("RayTracingScene must be initialized before setting instances");
    }

    for (const RayTracingInstanceDesc& instance : instances) {
        if (instance.blasIndex >= _blases.size() || instance.mask > 0xff) {
            throw std::out_of_range("RayTracingInstanceDesc references an invalid BLAS or instance mask");
        }
    }

    _instances.assign(instances.begin(), instances.end());
    _stats.instanceCount = static_cast<uint32_t>(_instances.size());
    mark_instances_dirty();
}

void RayTracingScene::record_builds(VkCommandBuffer cmd, uint32_t frameIndex)
{
    if (!_initialized || cmd == VK_NULL_HANDLE) {
        throw std::logic_error("RayTracingScene is not ready to record builds");
    }
    if (frameIndex >= _frames.size()) {
        throw std::out_of_range("RayTracingScene frame index is out of range");
    }

    const RayTracingFunctions functions = load_ray_tracing_functions(_device);
    RayTracingSceneFrameResources& frame = _frames[frameIndex];
    const bool buildStaticGeometry = _staticGeometryDirty;

    // BLAS and geometry metadata are shared by all frames. Replacing an existing
    // static scene is intentionally a rare, synchronized operation; dynamic
    // transforms remain frame-local and do not take this path.
    if (buildStaticGeometry) {
        const bool replacesExistingBLAS = std::any_of(
            _blases.begin(),
            _blases.end(),
            [](const RayTracingBLAS& blas) {
                return blas.accelerationStructure.handle != VK_NULL_HANDLE;
            });
        if (replacesExistingBLAS) {
            VK_CHECK(vkDeviceWaitIdle(_device));
        }
    }

    std::vector<PendingBLASBuild> pendingBLASBuilds;
    VkDeviceSize totalBLASScratchSize = 0;

    if (buildStaticGeometry) {
        if (_blases.size() != _meshDescs.size()) {
            _blases.resize(_meshDescs.size());
        }

        std::vector<GPURayTracingGeometry> gpuGeometries;
        uint32_t metadataOffset = 0;
        for (uint32_t meshIndex = 0; meshIndex < _meshDescs.size(); ++meshIndex) {
            const RayTracingMeshDesc& mesh = _meshDescs[meshIndex];
            RayTracingBLAS& blas = _blases[meshIndex];
            blas.sourceKey = mesh.sourceKey;
            blas.geometryMetadataOffset = metadataOffset;
            blas.geometryCount = static_cast<uint32_t>(mesh.geometries.size());
            blas.buildFlags = mesh.buildFlags;

            for (const RayTracingGeometryDesc& geometry : mesh.geometries) {
                GPURayTracingGeometry gpuGeometry{};
                gpuGeometry.vertexBufferAddress = geometry.vertexBufferAddress;
                gpuGeometry.indexBufferAddress = geometry.indexBufferAddress;
                gpuGeometry.firstIndex = geometry.firstIndex;
                gpuGeometry.indexCount = geometry.indexCount;
                gpuGeometry.vertexStride = geometry.vertexStride;
                gpuGeometry.materialID = geometry.materialID;
                gpuGeometry.flags = geometry.shaderFlags;
                gpuGeometries.push_back(gpuGeometry);
            }
            metadataOffset += blas.geometryCount;
        }

        if (!gpuGeometries.empty()) {
            const VkDeviceSize metadataSize =
                gpuGeometries.size() * sizeof(GPURayTracingGeometry);
            ensure_buffer_capacity(
                _allocator,
                _geometryMetadataBuffer,
                metadataSize,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VMA_MEMORY_USAGE_CPU_TO_GPU,
                true);
            std::memcpy(
                _geometryMetadataBuffer.info.pMappedData,
                gpuGeometries.data(),
                static_cast<size_t>(metadataSize));
            VK_CHECK(vmaFlushAllocation(
                _allocator, _geometryMetadataBuffer.allocation, 0, metadataSize));
        }

        pendingBLASBuilds.resize(_meshDescs.size());
        for (uint32_t meshIndex = 0; meshIndex < _meshDescs.size(); ++meshIndex) {
            const RayTracingMeshDesc& mesh = _meshDescs[meshIndex];
            PendingBLASBuild& pending = pendingBLASBuilds[meshIndex];
            pending.geometries.reserve(mesh.geometries.size());
            pending.ranges.reserve(mesh.geometries.size());
            pending.primitiveCounts.reserve(mesh.geometries.size());

            for (const RayTracingGeometryDesc& geometryDesc : mesh.geometries) {
                VkAccelerationStructureGeometryTrianglesDataKHR triangles{
                    VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
                triangles.vertexFormat = geometryDesc.vertexFormat;
                triangles.vertexData.deviceAddress = geometryDesc.vertexBufferAddress;
                triangles.vertexStride = geometryDesc.vertexStride;
                triangles.maxVertex = geometryDesc.vertexCount - 1;
                triangles.indexType = geometryDesc.indexType;
                triangles.indexData.deviceAddress = geometryDesc.indexBufferAddress;

                VkAccelerationStructureGeometryKHR geometry{
                    VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
                geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                geometry.geometry.triangles = triangles;
                geometry.flags = geometryDesc.buildFlags;
                pending.geometries.push_back(geometry);

                VkAccelerationStructureBuildRangeInfoKHR range{};
                range.primitiveCount = geometryDesc.indexCount / 3;
                range.primitiveOffset = geometryDesc.firstIndex * sizeof(uint32_t);
                pending.ranges.push_back(range);
                pending.primitiveCounts.push_back(range.primitiveCount);
            }

            pending.buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            pending.buildInfo.flags = mesh.buildFlags;
            pending.buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            pending.buildInfo.geometryCount = static_cast<uint32_t>(pending.geometries.size());
            pending.buildInfo.pGeometries = pending.geometries.data();

            functions.getBuildSizes(
                _device,
                VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                &pending.buildInfo,
                pending.primitiveCounts.data(),
                &pending.sizes);

            RayTracingBLAS& blas = _blases[meshIndex];
            destroy_acceleration_structure(
                functions, _device, _allocator, blas.accelerationStructure);
            blas.accelerationStructure = create_acceleration_structure(
                functions,
                _device,
                _allocator,
                VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                pending.sizes.accelerationStructureSize);
            pending.buildInfo.dstAccelerationStructure = blas.accelerationStructure.handle;

            pending.scratchOffset = align_up(totalBLASScratchSize, _scratchAlignment);
            totalBLASScratchSize = pending.scratchOffset + pending.sizes.buildScratchSize;
        }

        _stats.blasCount = static_cast<uint32_t>(_blases.size());
        _stats.geometryCount = metadataOffset;
    }

    const bool buildTLAS = !_instances.empty()
        && (frame.tlas.handle == VK_NULL_HANDLE
            || frame.builtGeometryGeneration != _geometryGeneration
            || frame.builtInstanceGeneration != _instanceGeneration);

    VkAccelerationStructureGeometryKHR tlasGeometry{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    VkAccelerationStructureBuildGeometryInfoKHR tlasBuildInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    VkAccelerationStructureBuildRangeInfoKHR tlasRange{};
    VkAccelerationStructureBuildSizesInfoKHR tlasSizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};

    if (buildTLAS) {
        const VkDeviceSize instanceBufferSize =
            _instances.size() * sizeof(VkAccelerationStructureInstanceKHR);
        const VkDeviceSize metadataBufferSize =
            _instances.size() * sizeof(GPURayTracingInstance);
        if (frame.instanceCapacity < _instances.size()) {
            destroy_buffer(_allocator, frame.instanceBuffer);
            destroy_buffer(_allocator, frame.instanceMetadataBuffer);
            frame.instanceBuffer = create_buffer(
                _allocator,
                instanceBufferSize,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                    | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                VMA_MEMORY_USAGE_CPU_TO_GPU,
                true);
            frame.instanceMetadataBuffer = create_buffer(
                _allocator,
                metadataBufferSize,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VMA_MEMORY_USAGE_CPU_TO_GPU,
                true);
            frame.instanceCapacity = static_cast<uint32_t>(_instances.size());
        }

        auto* vkInstances = static_cast<VkAccelerationStructureInstanceKHR*>(
            frame.instanceBuffer.info.pMappedData);
        auto* gpuInstances = static_cast<GPURayTracingInstance*>(
            frame.instanceMetadataBuffer.info.pMappedData);

        for (uint32_t instanceIndex = 0; instanceIndex < _instances.size(); ++instanceIndex) {
            const RayTracingInstanceDesc& instanceDesc = _instances[instanceIndex];
            if (instanceDesc.blasIndex >= _blases.size()) {
                throw std::out_of_range("Ray tracing instance references an invalid BLAS");
            }
            const RayTracingBLAS& blas = _blases[instanceDesc.blasIndex];
            if (blas.accelerationStructure.handle == VK_NULL_HANDLE) {
                throw std::logic_error("Ray tracing instance references an unbuilt BLAS");
            }
            if (instanceIndex > 0x00ffffffu) {
                throw std::overflow_error("Vulkan acceleration structure custom index exceeds 24 bits");
            }

            VkAccelerationStructureInstanceKHR& vkInstance = vkInstances[instanceIndex];
            vkInstance = {};
            vkInstance.transform = to_vk_transform(instanceDesc.transform);
            vkInstance.instanceCustomIndex = instanceIndex;
            vkInstance.mask = instanceDesc.mask;
            vkInstance.instanceShaderBindingTableRecordOffset = 0;
            vkInstance.flags = instanceDesc.flags;
            vkInstance.accelerationStructureReference = blas.accelerationStructure.deviceAddress;

            GPURayTracingInstance& gpuInstance = gpuInstances[instanceIndex];
            gpuInstance.geometryMetadataOffset = blas.geometryMetadataOffset;
            gpuInstance.instanceID = instanceDesc.instanceID;
            gpuInstance.flags = static_cast<uint32_t>(instanceDesc.flags);
            gpuInstance.padding = 0;
        }

        VK_CHECK(vmaFlushAllocation(
            _allocator, frame.instanceBuffer.allocation, 0, instanceBufferSize));
        VK_CHECK(vmaFlushAllocation(
            _allocator, frame.instanceMetadataBuffer.allocation, 0, metadataBufferSize));

        VkAccelerationStructureGeometryInstancesDataKHR instancesData{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
        instancesData.arrayOfPointers = VK_FALSE;
        instancesData.data.deviceAddress =
            get_buffer_address(_device, frame.instanceBuffer.buffer);

        tlasGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        tlasGeometry.geometry.instances = instancesData;

        tlasBuildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        tlasBuildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        tlasBuildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        tlasBuildInfo.geometryCount = 1;
        tlasBuildInfo.pGeometries = &tlasGeometry;

        const uint32_t primitiveCount = static_cast<uint32_t>(_instances.size());
        functions.getBuildSizes(
            _device,
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &tlasBuildInfo,
            &primitiveCount,
            &tlasSizes);

        if (frame.tlas.handle == VK_NULL_HANDLE
            || frame.tlasStorageCapacity < tlasSizes.accelerationStructureSize) {
            destroy_acceleration_structure(
                functions, _device, _allocator, frame.tlas);
            frame.tlas = create_acceleration_structure(
                functions,
                _device,
                _allocator,
                VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                tlasSizes.accelerationStructureSize);
            frame.tlasStorageCapacity = tlasSizes.accelerationStructureSize;
        }

        tlasBuildInfo.dstAccelerationStructure = frame.tlas.handle;
        tlasRange.primitiveCount = primitiveCount;
    }

    const VkDeviceSize requiredScratchSize = std::max(
        totalBLASScratchSize,
        buildTLAS ? tlasSizes.buildScratchSize : VkDeviceSize{0});
    if (requiredScratchSize > 0 && frame.scratchCapacity < requiredScratchSize) {
        destroy_buffer(_allocator, frame.scratchBuffer);
        frame.scratchBuffer = create_buffer(
            _allocator,
            requiredScratchSize + _scratchAlignment - 1,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY,
            false);
        frame.scratchCapacity = requiredScratchSize;
    }

    if (buildStaticGeometry || buildTLAS) {
        cmd_memory_barrier(
            cmd,
            VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_HOST_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
                | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR
                | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }

    if (!pendingBLASBuilds.empty()) {
        const VkDeviceAddress scratchAddress = align_up(
            get_buffer_address(_device, frame.scratchBuffer.buffer),
            _scratchAlignment);
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> buildInfos;
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rangePointers;
        buildInfos.reserve(pendingBLASBuilds.size());
        rangePointers.reserve(pendingBLASBuilds.size());

        for (PendingBLASBuild& pending : pendingBLASBuilds) {
            pending.buildInfo.scratchData.deviceAddress =
                scratchAddress + pending.scratchOffset;
            buildInfos.push_back(pending.buildInfo);
            rangePointers.push_back(pending.ranges.data());
        }

        functions.cmdBuildAccelerationStructures(
            cmd,
            static_cast<uint32_t>(buildInfos.size()),
            buildInfos.data(),
            rangePointers.data());

        cmd_memory_barrier(
            cmd,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    }

    if (buildTLAS) {
        const VkDeviceAddress scratchAddress = align_up(
            get_buffer_address(_device, frame.scratchBuffer.buffer),
            _scratchAlignment);
        tlasBuildInfo.scratchData.deviceAddress = scratchAddress;
        const VkAccelerationStructureBuildRangeInfoKHR* rangePointer = &tlasRange;
        functions.cmdBuildAccelerationStructures(
            cmd, 1, &tlasBuildInfo, &rangePointer);

        cmd_memory_barrier(
            cmd,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR
                | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

        frame.builtGeometryGeneration = _geometryGeneration;
        frame.builtInstanceGeneration = _instanceGeneration;
    }

    if (buildStaticGeometry) {
        _staticGeometryDirty = false;
        _stats.pendingBLASBuilds = 0;
    }
    _instancesDirty = false;
}

void RayTracingScene::mark_static_geometry_dirty()
{
    ++_geometryGeneration;
    _staticGeometryDirty = true;
    _stats.pendingBLASBuilds = static_cast<uint32_t>(_meshDescs.size());
}

void RayTracingScene::mark_instances_dirty()
{
    ++_instanceGeneration;
    _instancesDirty = true;
}

bool RayTracingScene::ready(uint32_t frameIndex) const
{
    if (!_initialized || frameIndex >= _frames.size() || _instances.empty()) {
        return false;
    }
    const RayTracingSceneFrameResources& frame = _frames[frameIndex];
    return frame.tlas.handle != VK_NULL_HANDLE
        && frame.builtGeometryGeneration == _geometryGeneration
        && frame.builtInstanceGeneration == _instanceGeneration;
}

VkAccelerationStructureKHR RayTracingScene::tlas(uint32_t frameIndex) const
{
    return frameIndex < _frames.size()
        ? _frames[frameIndex].tlas.handle
        : VK_NULL_HANDLE;
}

const AllocatedBuffer& RayTracingScene::instance_metadata_buffer(uint32_t frameIndex) const
{
    if (frameIndex >= _frames.size()) {
        throw std::out_of_range("RayTracingScene frame index is out of range");
    }
    return _frames[frameIndex].instanceMetadataBuffer;
}
