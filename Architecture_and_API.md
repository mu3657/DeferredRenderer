# DeferredRenderer 架构与 API 文档

本文档总结了 DeferredRenderer 当前代码库中关于核心渲染架构、数据结构、描述符 (Descriptor) 管理以及渲染流程的设计。

## 1. 核心 API 与数据结构

### 1.1 场景与节点渲染体系 (Scene & Nodes)
引擎采用节点树结构来组织和绘制渲染对象，支持变换矩阵的层级传递。基于面向对象接口设计。
*   `IRenderable` (vk_types.h): 基础渲染抽象接口，定义了 `virtual void Draw(const glm::mat4& topMatrix, DrawContext& ctx) = 0`。
*   `Node` (vk_types.h): 实现了 `IRenderable`。保存父节点弱引用和子节点列表，负责计算 `localTransform` 和 `worldTransform`，并递归调用子节点的绘制操作。
*   `MeshNode` (vk_types.h): 继承自 `Node`，包含对 `MeshAsset` 的引用，重写了 `Draw` 方法（实际将材质、网格与全局变换 Push 进 DrawContext）。
*   `LoadedGLTF` (vk_loader.h): 继承自 `IRenderable`。管理从 GLTF 加载的完整场景资源（Meshes, Nodes, Images, Materials），包含用于遍历的根节点表 `topNodes`。

### 1.2 GPU 数据类型与 Shader 输入绑定
与 Shader (`input_structures.glsl` / `mesh.vert` / `mesh.frag`) 直接对应的数据结构，保证 C++ 到 GPU 内存的一致性。

*   **Set 0: 全局/场景级数据 (Per-Frame)**
    *   `Binding 0` (UBO): `GPUSceneData` — 包含视图矩阵、投影矩阵、环境光、平行光方向和颜色。
    *   `Binding 1` (SSBO): `GPUObjectData` — 包含每个物体的模型矩阵、包围盒数据 (用于剔除)、颜色及自定义 ID。这是一个在 `FrameData` 中持久分配的大型 SSBO (`MAX_OBJECTS` 规模)。
*   **Set 1: 材质级数据 (Per-Material)**
    *   `Binding 0` (UBO): `GLTFMaterialData` / `MaterialConstants` — `colorFactors` 和 `metal_rough_factors` 及内存对齐 padding。
    *   `Binding 1` (Sampler): `colorTex` — 基础颜色贴图。
    *   `Binding 2` (Sampler): `metalRoughTex` — 金属粗糙度贴图。
*   **Push Constants (Per-Draw)**
    *   `GPUDrawPushConstants` — 包含 `worldMatrix` 与 `vertexBuffer` 地址（使用 Vulkan 1.2 `bufferDeviceAddress` 特性实现无绑定顶点缓冲拉取）。

### 1.3 核心渲染结构
*   **Frame/Time 结构**:
    *   `FrameData` (vk_engine.h): 记录单帧执行所需的 Command Pool/Buffer、同步原语 (Semaphore/Fence) 和该帧专用的动态分配器 (`_frameDescriptorsAllocator` / `_deletionQueue`)。
    *   重要修改：`objectStorageBuffer` (Binding 1) 和 `cameraBuffer` (Binding 0) 改为在 `FrameData` 中持久化分配（VMA_MEMORY_USAGE_CPU_TO_GPU），从而避免了每帧 `create_buffer/destroy_buffer` 的碎片化开销。并直接绑定于 `globalDescriptor` 中。
*   **DrawContext**:
    *   按材质透明度将绘制收集到 `OpaqueSurfaces` 或 `TransparentSurfaces` 列表中，解耦场景遍历和实际的 Command Buffer 录制。

---

## 2. 描述符 (Descriptor) 管理体系

系统封装了不同声明周期和用法的描述符分配机制。

### 2.1 描述符构建器 (DescriptorLayoutBuilder)
用于简明地构建 `VkDescriptorSetLayout`。通过调用 `add_binding(binding, type)` 附加绑定，最后调用 `build()` 创建布局。

### 2.2 描述符写入器 (DescriptorWriter)
提供流式的描述符更新方式。内部缓冲了 `VkDescriptorImageInfo` 和 `VkDescriptorBufferInfo` 对象的生命周期。
*   调用 `write_image` 和 `write_buffer` 记录待写入信息。
*   调用 `update_set()` 将所有收集的写入操作一次性提交给指定 Descriptor Set。

### 2.3 描述符分配器分类
1.  **全局/持久分配器 (`DescriptorAllocator globalDescriptorAllocator`)**
    *   **生命周期**: 随 Engine 初始化，在 Cleanup 时销毁。
    *   **用途**: 用于分配在整个程序运行期间都不会销毁的 Descriptor Set。例如：计算管线的输出图 (draw image)、所有材质的 Descriptor Set（`MaterialInstance` 中的 Set 1 材质参数和贴图）。
2.  **每帧增长型分配器 (`DescriptorAllocatorGrowable _frameDescriptorsAllocator`)**
    *   **生命周期**: 每个 `FrameData` 所独有。
    *   **用途**: 主要管理 UBO 等随帧动态生命周期的描述符绑定。采用满池自动创建新 Pool（指数扩容1.5倍），新老 Pool 缓冲在 `readyPools` 与 `fullPools` 的机制。每当这一帧被 GPU 渲染完成后（或者新一帧开始渲染前），会调用 `clear_pools` 高效重置所有 Pool 回归 `readyPools`。由于目前 Camera UBO 和 Object SSBO 已被固化，每帧的动态分配需求被显著降低，但此分配器依然是 ImGui 等第三方库或临时材质强有力的分配后盾。

---

## 3. 操作流程：创建、初始化与更新

### 3.1 Descriptor 的初始化与绑定流程
*(发生于 `VulkanEngine::init_descriptors()` 与帧初始化期间)*

1.  **定义 Layout 结构**:
    使用 `DescriptorLayoutBuilder` 构建大框架。例如 `_gpuSceneDataDescriptorLayout` 包含 1个 UBO (Binding 0) 和 1个 SSBO (Binding 1)。
2.  **创建持续缓冲区 (Buffer resources)**:
    引擎为当前叠加帧 `_frames[i]`，通过 `create_buffer` 预先分配 `cameraBuffer` (存放 `GPUSceneData`) 与 `objectStorageBuffer` (存放巨量 `GPUObjectData`)。
3.  **分配 Set 并建立长期映射**:
    使用 `_frameDescriptorsAllocator.allocate()` 为该帧分配出对应的 Descriptor Set (即 `globalDescriptor`)。
4.  **把 Buffer 注入 Descriptor (`DescriptorWriter`)**:
    使用 `DescriptorWriter` 将 `cameraBuffer` 写到 Binding 0，将 `objectStorageBuffer` 写到 Binding 1，并提交 `update_set`。
5.  **结果**: 帧启动后，CPU 能够通过 `allocation->GetMappedData()` 轻松向缓冲区写入场景数据，而此 Buffer 和 Descriptor Set 是已建立的长期信任关系，渲染时可以直接 Bind。

### 3.2 材质与流水线的创建流程
*(发生于 `GLTFMetallic_Roughness::build_pipelines` 与 `write_material`)*

1.  **资源准备**: 上传贴图（使用 `create_image` 到本地 GPU 内存）并生成 Mipmap，初始化 `VkSampler`。并准备 `MaterialConstants` 装载到 Uniform Buffer (UBO) 中。
2.  **创建 Pipeline Layout**: 取上方建好的 SceneData Descriptor Set Layout 和 Material Descriptor Set Layout (`materialLayout`) 进行合并，再加上 Push Constants 结构共同创建 `VkPipelineLayout`。
3.  **构建 Shader 流水线**: 用 `PipelineBuilder` 配置栅格化、深度测试、顶点和片段阶段。生成 `opaquePipeline` 及其变体 `transparentPipeline` (启用加法混合及关闭深度过滤)。
4.  **生成材质实例 (`write_material`)**:
    由全局分配器 `globalDescriptorAllocator` 分配出 Material Set。使用 `DescriptorWriter` 把前面创建的 UBO 数据绑在 Binding 0，基础颜色图与采样器绑在 Binding 1，金属粗糙图绑在 Binding 2。最终返回填充好管线指针和 Set 的 `MaterialInstance` 对象供场景节点引用。

### 3.3 场景绘制流程 (Draw Loop)
*(发生于 `VulkanEngine::update_scene` 和 `draw_geometry`)*

1.  **场景更新 (`update_scene`)**:
    计算相机视图和投影矩阵存入 `sceneData`；遍历树形结构的 Node 并用 `Draw` 提交渲染任务。所有的任务由各节点的 `MeshNode::Draw` 或者 `LoadedGLTF::Draw` 函数被注入到引擎统一的 `mainDrawContext`，透明/不透明分离。
2.  **CPU 数据填装**:
    在绘制前的 `draw_geometry` 里：
    将 `sceneData` 直接 `memcpy` 给 `currentFrame.cameraBuffer`。
    *(注：针对 SSBO 的具体单体更新逻辑正在完善，目前主要通过 GPU Push Constants 将逐个物体的局部变换推送给 GPU)*。
3.  **排序以优化状态切换**:
    通过匿名函数 `std::sort` 将 `OpaqueSurfaces` 依从 `Material` 和 `IndexBuffer` 指针地址排序，使底层状态绑定产生的开销最小化。
4.  **指令录制 (Command Recording)**:
    *   **全局绑定**: 在进入循环前无需反复绑定 Set 0。
    *   对于每一次 `draw()` (闭包内)：
        *   **检查材质状态**: 若材质改变，重新绑定 `vkCmdBindPipeline`，并绑定当前帧的全局描述符集 `globalDescriptor` (Set 0)，同时绑定该材质专属的描述符集 `r.material->materialSet` (Set 1)。
        *   **检查网格数据**: 针对变更绑定 `vkCmdBindIndexBuffer`。
        *   **局部属性更新**: 通过 `vkCmdPushConstants` 推送针对该 Mesh 的世界变换矩阵 (`worldMatrix`) 和无需描述符绑定的顶点缓冲区设备地址 `vertexBufferAddress`。
        *   提交绘制命令: `vkCmdDrawIndexed`。
5.  **收尾清理**:
    一帧构建并提交完毕后，清理 `mainDrawContext`，重置各类临时计量计数。并在安全释放时由 `_deletionQueue` 统一管理回收流程。
