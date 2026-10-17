#ifndef CLUSTER_GRID_GLSL
#define CLUSTER_GRID_GLSL

// params: near distance, far distance, slices / log2(far / near), slice count.
// Both passes use positive view-space distance, independent of reverse-Z.
uint clusterDepthSlice(float viewDepth, vec4 params)
{
    float slice = floor(log2(max(viewDepth, params.x) / params.x) * params.z);
    return uint(clamp(slice, 0.0, params.w - 1.0));
}

float clusterSliceBoundary(uint boundary, vec4 params)
{
    return params.x * exp2(float(boundary) / params.z);
}

uint clusterLinearIndex(uvec3 cluster, uvec2 tileCounts)
{
    return (cluster.z * tileCounts.y + cluster.y) * tileCounts.x + cluster.x;
}

#endif
