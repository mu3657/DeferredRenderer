
#pragma once

#include <cstdint>

#include <vk_types.h>

constexpr uint32_t MAX_GPU_LIGHTS = 256;

struct GPUSceneData {
    glm::mat4 view;
    glm::mat4 proj;
    glm::mat4 viewproj;
    glm::vec4 ambientColor;
    glm::vec4 sunlightDirection; // w for sun power
    glm::vec4 sunlightColor;
};


struct GPUObjectData {
    glm::mat4 modelMatrix;      // [0 - 64]  世界变换矩阵
    glm::vec4 origin_rad;       // [64 - 80] 包围球中心(xyz) + 半径(w) -> 用于粗略剔除
    glm::vec4 extents;          // [80 - 96] AABB 半尺寸 -> 用于精细剔除
    glm::vec4 color;            // [96 - 112] 调试颜色 / 基础颜色

    // [112 - 128] 填充与ID
    uint32_t objectID;          // 物体ID (用于鼠标拾取)
    uint32_t materialID;        // 材质ID (未来 Bindless 用)
    uint32_t padding1;          // 填充，保证 128 字节对齐
    uint32_t padding2;
};

struct GPULight {
    glm::vec4 positionRange;    // xyz = world position, w = range
    glm::vec4 directionType;    // xyz = light emission direction, w = LightType
    glm::vec4 colorIntensity;   // rgb = linear color, w = intensity
    glm::vec4 params;           // x/y = spot cone cosines, z = shadow index, w = flags
};

struct GPUShadowData {
    glm::mat4 lightViewProj;
    glm::vec4 lightDir;
    glm::vec4 params; // bias, normalBias, texelSize, enabled
};

struct GPULightData {
    uint32_t lightCount{0};
    uint32_t directionalLightCount{0};
    uint32_t pointLightCount{0};
    uint32_t spotLightCount{0};
    glm::vec4 ambientColor{0.1f};
};

static_assert(sizeof(GPULight) == 64);
static_assert(sizeof(GPULightData) == 32);
