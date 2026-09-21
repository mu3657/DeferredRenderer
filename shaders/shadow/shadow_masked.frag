#version 450

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "../input_structures.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 1) flat in uint inMaterialID;

void main()
{
    MaterialData material = materials[inMaterialID];
    float alpha = texture(
        globalTextures[nonuniformEXT(material.colorTexID)], inUV).a
        * material.colorFactors.a;
    if (alpha < clamp(material.emissive_factors.w, 0.0, 1.0)) {
        discard;
    }
}
