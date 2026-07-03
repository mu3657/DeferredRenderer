layout(set = 0, binding = 0) uniform  SceneData{

    mat4 view;
    mat4 proj;
    mat4 viewproj;
    vec4 ambientColor;
    vec4 sunlightDirection; //w for sun power--
    vec4 sunlightColor;
} sceneData;

layout(set = 0, binding = 1) uniform  ObjectBuffer{

    mat4 modelMatrix;      // [0 - 64]  世界变换矩阵
    vec4 origin_rad;       // [64 - 80] 包围球中心(xyz) + 半径(w) -> 用于粗略剔除
    vec4 extents;          // [80 - 96] AABB 半尺寸 -> 用于精细剔除
    vec4 color;            // [96 - 112] 调试颜色 / 基础颜色

// [112 - 128] 填充与ID
    uint objectID;          // 物体ID (用于鼠标拾取)
    uint materialID;        // 材质ID (未来 Bindless 用)
    uint padding1;          // 填充，保证 128 字节对齐
    uint padding2;
} ObjectData;

struct MaterialData {
    vec4 colorFactors;
    vec4 metal_rough_factors;
    vec4 emissive_factors;

    uint colorTexID;
    uint metalRoughTexID;
    uint normalTexID;
    uint occlusionTexID;

    uint emissiveTexID;
    uint pad0;
    uint pad1;
    uint pad2;

    vec4 extra[12];
};

#ifndef LIGHTING_PASS
layout(set = 1, binding = 0) readonly buffer MaterialStorage {
    MaterialData materials[];
};

layout(set = 1, binding = 1) uniform sampler2D globalTextures[];
#endif

#ifdef USE_LIGHT_DATA
const uint LIGHT_TYPE_POINT = 0;
const uint LIGHT_TYPE_DIRECTIONAL = 1;
const uint LIGHT_TYPE_SPOT = 2;

struct GPULight {
    vec4 positionRange;
    vec4 directionType;
    vec4 colorIntensity;
    vec4 params;
};

layout(set = 2, binding = 0) uniform LightData {
    uint lightCount;
    uint directionalLightCount;
    uint pointLightCount;
    uint spotLightCount;
    vec4 ambientColor;
} lightData;

layout(set = 2, binding = 1) readonly buffer LightStorage {
    GPULight lights[];
};
#endif
