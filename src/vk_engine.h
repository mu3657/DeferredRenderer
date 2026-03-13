// vulkan_guide.h : Include file for standard system include files,
// or project specific include files.

#pragma once

#include "vk_loader.h"
#include <vk_types.h>
#include <vk_initializers.h>
#include <vk_descriptors.h>

#include "vk_mem_alloc.h"
#include <GpuData.h>
//bootstrap library
#include "camera.h"
#include "VkBootstrap.h"
#include "vk_scene.h"

#if defined(_WIN32)
	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>
#elif defined(__linux__)
	#include <dlfcn.h>
#endif


#include "renderdoc_app.h"

struct DeletionQueue
{
	std::deque<std::function<void()>> deletors;

	void push_function(std::function<void()>&& function) {
		deletors.push_back(function);
	}

	void flush() {
		// reverse iterate the deletion queue to execute all the functions
		for (auto it = deletors.rbegin(); it != deletors.rend(); it++) {
			(*it)(); //call functors
		}

		deletors.clear();
	}
};

struct FrameData {

	VkCommandPool _commandPool;
	VkCommandBuffer _mainCommandBuffer;
	VkSemaphore _swapchainSemaphore;
	VkFence _renderFence;
	DeletionQueue _deletionQueue;
	DescriptorAllocatorGrowable _frameDescriptorsAllocator;



	// 1. 每帧独享的物体数据 SSBO (Binding 1)
	AllocatedBuffer objectStorageBuffer;

	// 2. 每帧独享的摄像机数据 UBO (Binding 0)

	AllocatedBuffer cameraBuffer;

	// 3. 指向上面两个 Buffer 的描述符集 (Set 0)
	VkDescriptorSet globalDescriptor;
};

struct ComputePushConstants {
	glm::vec4 data1;
	glm::vec4 data2;
	glm::vec4 data3;
	glm::vec4 data4;
};
struct ComputeEffect {
	const char* name;

	VkPipeline pipeline;
	VkPipelineLayout layout;

	ComputePushConstants data;
};
struct EngineStats {
	float frametime;
	int triangle_count;
	int drawcall_count;
	float scene_update_time;
	float mesh_draw_time;
};
class VulkanEngine;
struct GLTFMetallic_Roughness {
	MaterialPipeline opaquePipeline;
	MaterialPipeline transparentPipeline;

	VkDescriptorSetLayout materialLayout;

	struct MaterialConstants {
		glm::vec4 colorFactors;
		glm::vec4 metal_rough_factors;
		//padding, we need it anyway for uniform buffers
		glm::vec4 emissive_factors; // 发光因子 (rgb), w可存 emissive strength 或 alpha cutoff

		// 调整 padding 以保证正确的字节对齐 (比如满足 256 字节)
		glm::vec4 extra[13];
	};

	struct MaterialResources {
		AllocatedImage colorImage;
		VkSampler colorSampler;
		AllocatedImage metalRoughImage;
		VkSampler metalRoughSampler;

		AllocatedImage normalImage;         // 法线贴图
		VkSampler normalSampler;
		AllocatedImage occlusionImage;      // AO 遮蔽贴图
		VkSampler occlusionSampler;
		AllocatedImage emissiveImage;       // 发光贴图
		VkSampler emissiveSampler;


		VkBuffer dataBuffer;
		uint32_t dataBufferOffset;
	};

	DescriptorWriter writer;

	void build_pipelines(VulkanEngine* engine);
	void clear_resources(VkDevice device);

	MaterialInstance write_material(VkDevice device, MaterialPass pass, const MaterialResources& resources, DescriptorAllocatorGrowable& descriptorAllocator);
};
constexpr unsigned int FRAME_OVERLAP = 3;

class VulkanEngine {
public:

	bool _isInitialized{ false };
	int _frameNumber {0};
	bool stop_rendering{ false };
	VkExtent2D _windowExtent{ 1700 , 900 };

	struct SDL_Window* _window{ nullptr };

	VkInstance _instance;// Vulkan library handle
	VkDebugUtilsMessengerEXT _debug_messenger;// Vulkan debug output handle
	VkPhysicalDevice _chosenGPU;// GPU chosen as the default device
	VkDevice _device; // Vulkan device for commands
	VkSurfaceKHR _surface;// Vulkan window surface

	VkSwapchainKHR _swapchain;
	VkFormat _swapchainImageFormat;

	std::vector<VkImage> _swapchainImages;
	std::vector<VkImageView> _swapchainImageViews;
	VkExtent2D _swapchainExtent;

	FrameData _frames[FRAME_OVERLAP];
	std::vector<VkSemaphore> _renderSemaphores;
	FrameData& get_current_frame() { return _frames[_frameNumber % FRAME_OVERLAP]; };


	VkQueue _graphicsQueue;
	uint32_t _graphicsQueueFamily;

	DeletionQueue _mainDeletionQueue;

	VmaAllocator _allocator;

	AllocatedImage _drawImage;
	AllocatedImage _depthImage;

	AllocatedImage _gAlbedo; // 漫反射 (RGB) + 材质遮罩 (A)
	AllocatedImage _gNormal; // 世界空间法线 (RGB)
	AllocatedImage _gORM;    // AO (R), 粗糙度 (G), 金属度 (B)

	VkExtent2D _drawExtent;
	float renderScale{1.f};

	VkDescriptorSetLayout _gBufferDescriptorLayout;
	VkDescriptorSet _gBufferDescriptorSet;

	VkPipelineLayout _deferredLightingPipelineLayout;
	VkPipeline _deferredLightingPipeline;
	// --------------------------

	DescriptorAllocatorGrowable globalDescriptorAllocator;

	VkDescriptorSet _drawImageDescriptors;
	VkDescriptorSetLayout _drawImageDescriptorLayout;


	VkPipelineLayout _gradientPipelineLayout;

	// immediate submit structures
	VkFence _immFence;
	VkCommandBuffer _immCommandBuffer;
	VkCommandPool _immCommandPool;


	std::vector<ComputeEffect> backgroundEffects;
	int currentBackgroundEffect{0};

	VkPipelineLayout _trianglePipelineLayout;
	VkPipeline _trianglePipeline;

	std::vector<std::shared_ptr<MeshAsset>> testMeshes;
	MaterialInstance defaultData;
	GLTFMetallic_Roughness metalRoughMaterial;
	Camera mainCamera;
	AllocatedImage create_image(VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped = false);
	AllocatedImage create_image(void* data, VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped = false);

	AssetManager assetManager;
	RenderScene renderScene;

	std::unordered_map<std::string, std::shared_ptr<LoadedGLTF>> loadedScenes;

#pragma region DebugVariables
	EngineStats stats;
	RENDERDOC_API_1_1_2* rdoc_api = nullptr;
	bool capture_next_frame{ false };
	void init_renderdoc();
#pragma endregion


	void destroy_image(const AllocatedImage& img);



	void init_triangle_pipeline();

	void geometry_pass(VkCommandBuffer cmd);
	void lighting_pass(VkCommandBuffer cmd, VkImageView targetImageView);

	static VulkanEngine& Get();

	//initializes everything in the engine
	void init();

	//shuts down the engine
	void cleanup();
	void draw_background(VkCommandBuffer cmd);

	//draw loop
	void draw();

	//run main loop
	void run();
	void immediate_submit(std::function<void(VkCommandBuffer cmd)>&& function);

	AllocatedBuffer create_buffer(size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage);

	void destroy_buffer(const AllocatedBuffer &buffer);


	GPUMeshBuffers uploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices);


	bool resize_requested{ false };

	GPUSceneData sceneData;

	VkDescriptorSetLayout _gpuSceneDataDescriptorLayout;


	//tex
	AllocatedImage _whiteImage;
	AllocatedImage _blackImage;
	AllocatedImage _greyImage;
	AllocatedImage _errorCheckerboardImage;
	AllocatedImage _defaultNormalImage; // NEW

	VkSampler _defaultSamplerLinear;
	VkSampler _defaultSamplerNearest;


	DrawContext mainDrawContext;
	std::unordered_map<std::string, std::shared_ptr<Node>> loadedNodes;

	void update_scene();
	void init_camera();
private:
	void init_vulkan();
	void draw_imgui(VkCommandBuffer cmd, VkImageView targetImageView);
	void init_swapchain();
	void init_gbuffer();
	void init_commands();
	void init_sync_structures();
	void init_descriptors();
	void create_swapchain(uint32_t width, uint32_t height);
	void destroy_swapchain();

	void resize_swapchain();

	void init_pipelines();
	void init_background_pipelines();
	void init_Deferredlighting_pipeline();

	void init_default_data();

	void init_imgui();

};
