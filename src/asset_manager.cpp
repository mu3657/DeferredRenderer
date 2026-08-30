#include "asset_manager.h"
#include "vk_engine.h"
#include <mesh_asset.h>
#include "fmt/core.h"
#include <vk_types.h>
#include <iostream>
#include <prefab_asset.h>
#include <material_asset.h>
#include <texture_asset.h>
#include <vk_loader.h>
#include <algorithm>
void AssetManager::init(VulkanEngine* engine) {
    _engine = engine;
    // Build a default material wrapping the engine's pre-allocated defaultData
    _defaultMaterial = std::make_shared<Material>();
    _defaultMaterial->data = engine->defaultData;
}

void AssetManager::cleanup() {
    for (auto& [path, mesh] : _meshes) {
        _engine->destroy_buffer(mesh->meshBuffers.indexBuffer);
        _engine->destroy_buffer(mesh->meshBuffers.vertexBuffer);
    }
    _meshes.clear();
}

std::shared_ptr<MeshAsset> AssetManager::load_mesh(const std::string& path) {
    auto it = _meshes.find(path);
    if (it != _meshes.end()) {
        return it->second;
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

    // 计算顶点数量
    uint32_t vertexCount = 0;
    if (meshInfo.vertexFormat == assets::VertexFormat::Dynamic) {
        vertexCount = meshInfo.vertexBuferSize / meshInfo.vertexStride;
    } else if (meshInfo.vertexFormat == assets::VertexFormat::PNCV_F32) {
        vertexCount = meshInfo.vertexBuferSize / sizeof(assets::Vertex_f32_PNCV);
    } else if (meshInfo.vertexFormat == assets::VertexFormat::P32N8C8V16) {
        vertexCount = meshInfo.vertexBuferSize / sizeof(assets::Vertex_P32N8C8V16);
    }

    uint32_t indexCount = meshInfo.indexBuferSize / meshInfo.indexSize;
    if (meshInfo.indexSize != sizeof(uint32_t)
        || (meshInfo.indexBuferSize % sizeof(uint32_t)) != 0) {
        throw std::runtime_error(
            fmt::format("Mesh {} does not contain tightly packed uint32 indices", path));
    }

    const auto* meshIndices = reinterpret_cast<const uint32_t*>(indexBuffer.data());
    for (uint32_t index = 0; index < indexCount; ++index) {
        if (meshIndices[index] >= vertexCount) {
            throw std::out_of_range(fmt::format(
                "Mesh {} index {} references vertex {}, but vertexCount is {}",
                path,
                index,
                meshIndices[index],
                vertexCount));
        }
    }

    // -----------------------------------------------------------------------
    // 格式转换：baker 存的是 Vertex_f32_PNCV (44 bytes, P/N/Color3/UV 顺序)，
    // 而 gbuffer.vert 通过 buffer_reference std430 期望 engine::Vertex (48 bytes,
    // position/uv_x/normal/uv_y/color4 交叉格式)。
    // VertexIndex > 0 时 stride 不同会导致 shader 从错误偏移读 position，
    // 使 gl_Position 全部错误。在上传 GPU 前先在 CPU 侧做显式转换。
    // -----------------------------------------------------------------------
    std::vector<Vertex> engineVertices(vertexCount);

    if (meshInfo.vertexFormat == assets::VertexFormat::PNCV_F32) {
        const auto* src = reinterpret_cast<const assets::Vertex_f32_PNCV*>(vertexBuffer.data());
        for (uint32_t i = 0; i < vertexCount; i++) {
            engineVertices[i].position = { src[i].position[0], src[i].position[1], src[i].position[2] };
            engineVertices[i].normal   = { src[i].normal[0],   src[i].normal[1],   src[i].normal[2] };
            engineVertices[i].color    = { src[i].color[0], src[i].color[1], src[i].color[2], 1.0f };
            engineVertices[i].uv_x    = src[i].uv[0];
            engineVertices[i].uv_y    = src[i].uv[1];
        }
    } else if (meshInfo.vertexFormat == assets::VertexFormat::P32N8C8V16) {
        const auto* src = reinterpret_cast<const assets::Vertex_P32N8C8V16*>(vertexBuffer.data());
        for (uint32_t i = 0; i < vertexCount; i++) {
            engineVertices[i].position = { src[i].position[0], src[i].position[1], src[i].position[2] };
            engineVertices[i].normal   = {
                (src[i].normal[0] / 127.5f) - 1.0f,
                (src[i].normal[1] / 127.5f) - 1.0f,
                (src[i].normal[2] / 127.5f) - 1.0f
            };
            engineVertices[i].color    = {
                src[i].color[0] / 255.0f,
                src[i].color[1] / 255.0f,
                src[i].color[2] / 255.0f,
                1.0f
            };
            engineVertices[i].uv_x = src[i].uv[0];
            engineVertices[i].uv_y = src[i].uv[1];
        }
    } else {
        //未知格式：直接 memcpy

        size_t copySize = std::min(vertexCount * sizeof(Vertex), vertexBuffer.size());
        memcpy(engineVertices.data(), vertexBuffer.data(), copySize);
    }

    // 创建 MeshAsset
    auto newMesh = std::make_shared<MeshAsset>();
    newMesh->name = path;

    GeoSurface surface;
    surface.startIndex = 0;
    surface.count = indexCount;
    surface.material = nullptr;
    // 从 baker 计算好的 MeshInfo::bounds 填入正确包围体
    // 若不填，is_visible 的 8 个角点全在 (0,0,0)，退化为单点剔除，
    // 相同网格在不同位置有的被错误剔除，有的正常
    surface.bounds.origin       = { meshInfo.bounds.origin[0], meshInfo.bounds.origin[1], meshInfo.bounds.origin[2] };
    surface.bounds.sphereRadius = meshInfo.bounds.radius;
    surface.bounds.extents      = { meshInfo.bounds.extents[0], meshInfo.bounds.extents[1], meshInfo.bounds.extents[2] };
    newMesh->surfaces.push_back(surface);

    const size_t uploadVertexSize = engineVertices.size() * sizeof(Vertex);
    const size_t indexBufferSize  = indexBuffer.size();

    auto& engine = *_engine;
    newMesh->meshBuffers.vertexBuffer = engine.create_buffer(
        uploadVertexSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
            | VK_BUFFER_USAGE_TRANSFER_DST_BIT
            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
            | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        VMA_MEMORY_USAGE_GPU_ONLY);

    VkBufferDeviceAddressInfo deviceAddressInfo{ .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = newMesh->meshBuffers.vertexBuffer.buffer };
    newMesh->meshBuffers.vertexBufferAddress = vkGetBufferDeviceAddress(engine._device, &deviceAddressInfo);

    newMesh->meshBuffers.indexBuffer = engine.create_buffer(
        indexBufferSize,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT
            | VK_BUFFER_USAGE_TRANSFER_DST_BIT
            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
            | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        VMA_MEMORY_USAGE_GPU_ONLY);

    deviceAddressInfo.buffer = newMesh->meshBuffers.indexBuffer.buffer;
    newMesh->meshBuffers.indexBufferAddress = vkGetBufferDeviceAddress(engine._device, &deviceAddressInfo);
    newMesh->meshBuffers.vertexCount = static_cast<uint32_t>(engineVertices.size());
    newMesh->meshBuffers.indexCount = indexCount;

    AllocatedBuffer staging = engine.create_buffer(
        uploadVertexSize + indexBufferSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_CPU_ONLY);
    void* data = staging.info.pMappedData;

    memcpy(data, engineVertices.data(), uploadVertexSize);
    memcpy((char*)data + uploadVertexSize, indexBuffer.data(), indexBufferSize);

    engine.immediate_submit([&](VkCommandBuffer cmd) {
        VkBufferCopy vertexCopy{ 0 };
        vertexCopy.size = uploadVertexSize;
        vkCmdCopyBuffer(cmd, staging.buffer, newMesh->meshBuffers.vertexBuffer.buffer, 1, &vertexCopy);

        VkBufferCopy indexCopy{ 0 };
        indexCopy.srcOffset = uploadVertexSize;
        indexCopy.size = indexBufferSize;
        vkCmdCopyBuffer(cmd, staging.buffer, newMesh->meshBuffers.indexBuffer.buffer, 1, &indexCopy);
    });

    engine.destroy_buffer(staging);

    _meshes[path] = newMesh;
    return newMesh;
}

std::shared_ptr<LoadedScene> AssetManager::load_prefab(const std::string& path) {
    assets::AssetFile file;
    if (!assets::load_binaryfile(path.c_str(), file)) {
        fmt::print("Error: Could not load prefab asset {}\n", path);
        return nullptr;
    }

    assets::PrefabInfo prefabInfo = assets::read_prefab_info(&file);

    std::shared_ptr<LoadedScene> scene = std::make_shared<LoadedScene>();
    scene->creator = _engine;

    std::filesystem::path prefabFolder = std::filesystem::path(path).parent_path();

    std::unordered_map<uint64_t, std::shared_ptr<Node>> nodeMap;

    // First pass: Create all nodes
    for (auto& [nodeID, name] : prefabInfo.node_names) {
        std::shared_ptr<Node> newNode;

        // Does this node have a mesh?
        if (prefabInfo.node_meshes.find(nodeID) != prefabInfo.node_meshes.end()) {
            newNode = std::make_shared<MeshNode>();
            auto meshComp = static_cast<MeshNode*>(newNode.get());
            
            // Resolve Mesh
            std::string meshPath = (prefabFolder / prefabInfo.node_meshes[nodeID].mesh_path).string();
            std::shared_ptr<MeshAsset> mAsset = load_mesh(meshPath);
            if(mAsset) {
                meshComp->mesh = mAsset;
            }

            // Resolve Material
            std::string matPath = (prefabFolder / prefabInfo.node_meshes[nodeID].material_path).string();
            std::shared_ptr<Material> mat = load_material(matPath);
            
            // Assign material to all surfaces of the mesh instance attached to this node
            if (mAsset && mat) {
                // Since `mAsset` is shared across potentially many nodes, we can't safely mutate `mAsset->surfaces` material pointers directly here
                // without affecting other instances. 
                // Wait, in `vk_loader.h`, `GeoSurface` belongs to `MeshAsset`. This means materials are currently intrinsically bound to the MeshAsset geometry.
                // We'll bind the material to the mesh's surfaces directly for now, which assumes a 1:1 mapping of MeshAsset to Material for simple cases.
                // A more robust engine design separates Mesh Geometry and Material instances at the Node level.
                for(auto& surface : mAsset->surfaces) {
                    if (surface.material == nullptr) {
                        surface.material = mat;
                    }
                }
            }
        } else if (prefabInfo.node_lights.find(nodeID) != prefabInfo.node_lights.end()) {
            const auto& nl = prefabInfo.node_lights.at(nodeID);
            auto lightNode = std::make_shared<LightNode>();
            lightNode->light.color     = glm::vec3(nl.color[0], nl.color[1], nl.color[2]);
            lightNode->light.intensity = nl.intensity;
            lightNode->light.type      = static_cast<LightType>(nl.type);
            lightNode->light.range     = nl.range;
            lightNode->light.width     = nl.width;
            lightNode->light.height    = nl.height;
            // worldPosition will be computed during Draw() when the transform is known;
            // store a weak_ptr so the lighting pass can read the up-to-date GpuLight each frame.
            scene->lightNodes.push_back(lightNode);
            newNode = lightNode;
        } else {
            newNode = std::make_shared<Node>();
        }

        newNode->name = name;
        scene->nodes[name] = newNode;
        nodeMap[nodeID] = newNode;
    }

    // Pass 1: Set all localTransforms first (unordered_map iteration order is random,
    //         so we must NOT call refreshTransform until every node has its localTransform).
    for (auto& [nodeID, newNode] : nodeMap) {
        if (prefabInfo.node_matrices.find(nodeID) != prefabInfo.node_matrices.end()) {
            int matIdx = prefabInfo.node_matrices.at(nodeID);
            const std::array<float, 16>& matrix = prefabInfo.matrices[matIdx];
            memcpy(&newNode->localTransform, matrix.data(), sizeof(glm::mat4));
        } else {
            // 没有存储矩阵（如 baker 里多 primitive 分裂的子节点）：显式设为 identity
            // glm::mat4 默认构造 在 value-initialization 下可能是全零矩阵，而不是 identity
            newNode->localTransform = glm::mat4{1.f};
        }
    }

    // Pass 2: Wire up parent-child relationships (no transform recalculation yet)
    for (auto& [nodeID, newNode] : nodeMap) {
        if (prefabInfo.node_parents.find(nodeID) != prefabInfo.node_parents.end()) {
            uint64_t parentID = prefabInfo.node_parents.at(nodeID);
            auto parentIt = nodeMap.find(parentID);
            if (parentIt != nodeMap.end()) {
                parentIt->second->children.push_back(newNode);
                newNode->parent = parentIt->second;
            }
        } else {
            scene->topNodes.push_back(newNode);
        }
    }

    // Pass 3: Propagate world transforms top-down (all localTransforms and parent links are set)
    for (auto& topNode : scene->topNodes) {
        topNode->refreshTransform(glm::mat4{1.f});
    }

    return scene;
}

#include <sstream>

glm::vec4 parse_vec4(const std::string& str, glm::vec4 defaultVal) {
    std::stringstream ss(str);
    glm::vec4 v = defaultVal;
    if (!(ss >> v.x >> v.y >> v.z >> v.w)) {
        return defaultVal;
    }
    return v;
}

glm::vec3 parse_vec3(const std::string& str, glm::vec3 defaultVal) {
    std::stringstream ss(str);
    glm::vec3 v = defaultVal;
    if (!(ss >> v.x >> v.y >> v.z)) {
        return defaultVal;
    }
    return v;
}

float parse_float(const std::string& str, float defaultVal) {
    try {
        return std::stof(str);
    } catch (...) {
        return defaultVal;
    }
}

std::shared_ptr<Material> AssetManager::load_material(const std::string& path) {
    auto it = _materials.find(path);
    if (it != _materials.end()) {
        return it->second;
    }

    assets::AssetFile file;
    if (!assets::load_binaryfile(path.c_str(), file)) {
        fmt::print("Warning: Could not load material asset {}, using default material\n", path);
        return _defaultMaterial;
    }

    assets::MaterialInfo matInfo = assets::read_material_info(&file);

    MaterialSurface surface = MaterialSurface::Opaque;
    if (matInfo.transparency == assets::TransparencyMode::Transparent) {
        surface = MaterialSurface::Transparent;
    }

    // Resolve Textures
    std::filesystem::path matFolder = std::filesystem::path(path).parent_path();


    std::filesystem::path exportRoot = matFolder.parent_path();


    auto get_texture = [&](const std::string& name, AllocatedImage fallback, bool srgb) -> AllocatedImage {
        auto it = matInfo.textures.find(name);
        if (it != matInfo.textures.end()) {
            std::string texPath = (exportRoot / it->second).lexically_normal().generic_string();
            return load_texture(texPath, srgb);
        }
        return fallback;
    };

    GLTFMetallic_Roughness::MaterialResources resources;
    resources.colorImage        = get_texture("baseColor", _engine->_whiteImage, true);
    resources.colorSampler      = _engine->_defaultSamplerLinear;
    // glTF factors are multiplied by the texture, so a missing texture must be neutral.
    resources.metalRoughImage   = get_texture("metallicRoughness", _engine->_whiteImage, false);
    resources.metalRoughSampler = _engine->_defaultSamplerLinear;
    resources.normalImage       = get_texture("normals", _engine->_defaultNormalImage, false);
    resources.normalSampler     = _engine->_defaultSamplerLinear;
    resources.occlusionImage    = get_texture("occlusion", _engine->_whiteImage, false);
    resources.occlusionSampler  = _engine->_defaultSamplerLinear;
    resources.emissiveImage     = get_texture("emissive", _engine->_blackImage, true);
    resources.emissiveSampler   = _engine->_defaultSamplerLinear;

    // Parse Factors
    GLTFMetallic_Roughness::MaterialConstants constants{};
    constants.colorFactors = parse_vec4(matInfo.customProperties["baseColorFactor"], glm::vec4(1.0f));

    // Some legacy materials carry a meaningful alpha but were exported as
    // OPAQUE. Use the authored alpha only; material names are not a domain rule.
    if (surface == MaterialSurface::Opaque
        && constants.colorFactors.a < 0.999f) {
        surface = MaterialSurface::Transparent;
    }
    
    float metallic = parse_float(matInfo.customProperties["metallicFactor"], 1.0f);
    float roughness = parse_float(matInfo.customProperties["roughnessFactor"], 1.0f);
    constants.metal_rough_factors = glm::vec4(metallic, roughness, 0.0f, 0.0f);

    constants.emissive_factors = glm::vec4(
        parse_vec3(matInfo.customProperties["emissiveFactor"], glm::vec3(0.0f)),
        parse_float(matInfo.customProperties["alphaCutoff"], 0.5f));

    // Pass constants struct directly to write_material for SSBO inclusion
    resources.data = constants;

    // Build Material Instance
    MaterialInstance matInstance = _engine->metalRoughMaterial.write_material(_engine->_device, surface, resources, _engine->globalDescriptorAllocator);

    auto newMat = std::make_shared<Material>();
    newMat->data = matInstance;

    _materials[path] = newMat;
    return newMat;
}

AllocatedImage AssetManager::load_texture(const std::string& path, bool srgb) {
    const std::string cacheKey = path + (srgb ? "#srgb" : "#linear");
    auto it = _textures.find(cacheKey);
    if (it != _textures.end()) {
        return it->second;
    }

    assets::AssetFile file;
    if (!assets::load_binaryfile(path.c_str(), file)) {
        fmt::print("Error: Could not load texture asset {}\n", path);
        return _engine->_errorCheckerboardImage;
    }

    assets::TextureInfo txInfo = assets::read_texture_info(&file);

    // Simplification: We decode page 0 only for immediate display (assuming no complex mip chains serialized here right now)
    // Real implementation would unpack and push via Vulkan buffer -> Image copy staging.
    // For now, since `vk_loader.cpp` handled `stbi_load` and image creation, this needs equivalent image generation logic.
    // That means unpacking the LZ4 texture page to raw RGBA arrays, then calling engine->create_image.

    if (txInfo.pages.empty()) {
        fmt::print("Warning: Texture {} has 0 pages! Returning fallback image.\n", path);
        // 返回引擎里默认的紫黑棋盘格
        return _engine->_errorCheckerboardImage;
    }


    std::vector<char> rawImagePixels(txInfo.pages[0].originalSize);
    assets::unpack_texture_page(&txInfo, 0, file.binaryBlob.data(), rawImagePixels.data());

    VkExtent3D extents;
    extents.width = txInfo.pages[0].width;
    extents.height = txInfo.pages[0].height;
    extents.depth = 1;

    const VkFormat imageFormat = srgb
        ? VK_FORMAT_R8G8B8A8_SRGB
        : VK_FORMAT_R8G8B8A8_UNORM;
    AllocatedImage newImage = _engine->create_image(
        rawImagePixels.data(),
        extents,
        imageFormat,
        VK_IMAGE_USAGE_SAMPLED_BIT,
        true);
    
    _textures[cacheKey] = newImage;

    _engine->_mainDeletionQueue.push_function([=, engine = _engine]() {
            _engine->destroy_image(newImage);

});
    return newImage;
}
