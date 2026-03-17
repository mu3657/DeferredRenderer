#include "vk_loader.h"
#include "vk_engine.h"
#include "vk_initializers.h"
#include "vk_types.h"
#include <iostream>
#include <fmt/core.h>
#include "asset_manager.h"

std::optional<std::shared_ptr<LoadedScene>> loadScene(VulkanEngine* engine, std::string_view filePath)
{
    fmt::print("Loading Scene: {}\n", filePath);

    std::shared_ptr<LoadedScene> scene = engine->assetManager.load_prefab(std::string(filePath));
    if (!scene) {
        fmt::print("Failed to load scene: {}\n", filePath);
        return {};
    }

    return scene;
}

void LoadedScene::Draw(const glm::mat4& topMatrix, DrawContext& ctx)
{
    // create renderables from the scenenodes
    for (auto& n : topNodes) {
        n->Draw(topMatrix, ctx);
    }
}

void LoadedScene::clearAll()
{
    VkDevice dv = creator->_device;

    descriptorPool.destroy_pools(dv);
    creator->destroy_buffer(materialDataBuffer);

    for (auto& sampler : samplers) {
        vkDestroySampler(dv, sampler, nullptr);
    }
}