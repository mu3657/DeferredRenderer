#include "asset_manager.h"
#include "vk_engine.h"
#include <mesh_asset.h>
#include "fmt/core.h"
#include <vk_types.h>
#include <iostream>

void AssetManager::init(VulkanEngine* engine) {
    _engine = engine;
}

void AssetManager::cleanup() {
    for (auto& [path, mesh] : _meshes) {
        _engine->destroy_buffer(mesh->meshBuffers.indexBuffer);
        _engine->destroy_buffer(mesh->meshBuffers.vertexBuffer);
    }
    _meshes.clear();
}

MeshAsset* AssetManager::load_mesh(const std::string& path) {
    auto it = _meshes.find(path);
    if (it != _meshes.end()) {
        return it->second.get();
    }

    assets::AssetFile file;
    if (!assets::load_binaryfile(path.c_str(), file)) {
        fmt::print("Error: Could not load mesh asset {}\n", path);
        return nullptr;
    }

    assets::MeshInfo meshInfo = assets::read_mesh_info(&file);

    std::vector<char> vertexBuffer(meshInfo.vertexBuferSize);
    std::vector<char> indexBuffer(meshInfo.indexBuferSize);

    assets::unpack_mesh(&meshInfo, file.binaryBlob.data(), file.binaryBlob.size(), vertexBuffer.data(), indexBuffer.data());

    // In a full implementation, you'd use meshInfo.attributes to dynamically
    // construct Vulkan VkVertexInputAttributeDescription and bindings.
    // For now, we upload raw bytes and count on the Pipeline definition to match.

    // Calculate vertex count and index count
    uint32_t vertexCount = 0;
    if (meshInfo.vertexFormat == assets::VertexFormat::Dynamic) {
        vertexCount = meshInfo.vertexBuferSize / meshInfo.vertexStride;
    } else if (meshInfo.vertexFormat == assets::VertexFormat::PNCV_F32) {
        vertexCount = meshInfo.vertexBuferSize / sizeof(assets::Vertex_f32_PNCV);
    } else if (meshInfo.vertexFormat == assets::VertexFormat::P32N8C8V16) {
        vertexCount = meshInfo.vertexBuferSize / sizeof(assets::Vertex_P32N8C8V16);
    }

    uint32_t indexCount = meshInfo.indexBuferSize / meshInfo.indexSize;

    // Create a new mesh asset
    auto newMesh = std::make_unique<MeshAsset>();
    newMesh->name = path;

    GeoSurface surface;
    surface.startIndex = 0;
    surface.count = indexCount;
    surface.material = nullptr;
    newMesh->surfaces.push_back(surface);

    const size_t vertexBufferSize = vertexBuffer.size();
    const size_t indexBufferSize = indexBuffer.size();

    auto& engine = *_engine;
    newMesh->meshBuffers.vertexBuffer = engine.create_buffer(vertexBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

    VkBufferDeviceAddressInfo deviceAddressInfo{ .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = newMesh->meshBuffers.vertexBuffer.buffer };
    newMesh->meshBuffers.vertexBufferAddress = vkGetBufferDeviceAddress(engine._device, &deviceAddressInfo);

    newMesh->meshBuffers.indexBuffer = engine.create_buffer(indexBufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

    AllocatedBuffer staging = engine.create_buffer(vertexBufferSize + indexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
    void* data = staging.info.pMappedData;

    memcpy(data, vertexBuffer.data(), vertexBufferSize);
    memcpy((char*)data + vertexBufferSize, indexBuffer.data(), indexBufferSize);

    engine.immediate_submit([&](VkCommandBuffer cmd) {
        VkBufferCopy vertexCopy{ 0 };
        vertexCopy.size = vertexBufferSize;
        vkCmdCopyBuffer(cmd, staging.buffer, newMesh->meshBuffers.vertexBuffer.buffer, 1, &vertexCopy);

        VkBufferCopy indexCopy{ 0 };
        indexCopy.srcOffset = vertexBufferSize;
        indexCopy.size = indexBufferSize;
        vkCmdCopyBuffer(cmd, staging.buffer, newMesh->meshBuffers.indexBuffer.buffer, 1, &indexCopy);
    });

    engine.destroy_buffer(staging);

    MeshAsset* result = newMesh.get();
    _meshes[path] = std::move(newMesh);

    return result;
}
