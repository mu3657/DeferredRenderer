
#include <vk_types.h>
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