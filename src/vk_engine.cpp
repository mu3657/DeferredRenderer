//> includes
#define VMA_IMPLEMENTATION
#include "vk_engine.h"
#include <Tracy/Tracy.hpp>


#include <glm/gtx/transform.hpp>
#include <SDL.h>
#include <SDL_vulkan.h>

#include <vk_initializers.h>
#include <vk_types.h>

#include <chrono>
#include <thread>

#include "fmt/color.h"
#include <vk_images.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_vulkan.h"

#include "vk_pipelines.h"
VulkanEngine* loadedEngine = nullptr;
#ifdef NDEBUG
const bool bUseValidationLayers = false;
#else
const bool bUseValidationLayers = true;
#endif

const size_t MAX_OBJECTS = 10000;
VulkanEngine& VulkanEngine::Get() { return *loadedEngine; }




void VulkanEngine::init()
{

	loadedEngine = this;
    // We initialize SDL and create a window with it.
    SDL_Init(SDL_INIT_VIDEO);

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    _window = SDL_CreateWindow(
        "Vulkan Engine",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        _windowExtent.width,
        _windowExtent.height,
        window_flags
    );
	//init_renderdoc();
    init_vulkan();

    init_swapchain();

    init_commands();

    init_sync_structures();

    init_descriptors();
    init_default_data();
    init_gbuffer(); 
    init_pipelines();
    init_imgui();

	init_camera();
	assetManager.init(this);
	//std::string structurePath = { "../assets/BistroInterior_out/BistroInterior.gltf" };
	 std::string structurePath = { "../assets/BistroInterior_out/assets_export/BistroInterior.pfb" };
	//std::string structurePath = { "../assets/structure/assets_export/structure.pfb" };
	auto structureFile = loadScene(this,structurePath);

	if (structureFile.has_value()) {
	    loadedScenes["structure"] = *structureFile;
	}

    //everything went fine
    _isInitialized = true;


}
void VulkanEngine::init_renderdoc() {
	// 1. 动态加载 RenderDoc 动态链接库
#if defined(_WIN32)
	HMODULE mod = LoadLibraryA("renderdoc.dll");
	if (!mod) {
		mod = LoadLibraryA("D:\\TA\\newestRD\\Pro_2.0_RenderDoc_1.36_x64\\RenderDoc_Pro_1.36_64\\renderdoc.dll");
	}
#elif defined(__linux__)
	void *mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
#endif

	if (!mod) {
		fmt::println("RenderDoc not found. Running without graphics debugger.");
		return;
	}

	// 2. 获取 API 入口点
	pRENDERDOC_GetAPI RENDERDOC_GetAPI =
#if defined(_WIN32)
		(pRENDERDOC_GetAPI)GetProcAddress(mod, "RENDERDOC_GetAPI");
#elif defined(__linux__)
			(pRENDERDOC_GetAPI)dlsym(mod, "RENDERDOC_GetAPI");
#endif

	// 3. 初始化 API 结构体
	int ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_1_2, (void **)&rdoc_api);
	if (ret != 1) {
		fmt::println("RenderDoc API initialization failed!");
	} else {
		fmt::println("RenderDoc API successfully loaded!");
		// 屏蔽 RenderDoc 默认的屏幕左上角提示文字（可选）
		rdoc_api->SetCaptureOptionU32(eRENDERDOC_Option_DebugOutputMute, 1);
	}
}
void VulkanEngine::init_imgui()
{
	// 1: create descriptor pool for IMGUI
	//  the size of the pool is very oversize, but it's copied from imgui demo
	//  itself.
	VkDescriptorPoolSize pool_sizes[] = { { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 } };

	VkDescriptorPoolCreateInfo pool_info = {};
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pool_info.maxSets = 1000;
	pool_info.poolSizeCount = (uint32_t)std::size(pool_sizes);
	pool_info.pPoolSizes = pool_sizes;

	VkDescriptorPool imguiPool;
	VK_CHECK(vkCreateDescriptorPool(_device, &pool_info, nullptr, &imguiPool));

	// 2: initialize imgui library

	// this initializes the core structures of imgui
	ImGui::CreateContext();

	// this initializes imgui for SDL
	ImGui_ImplSDL2_InitForVulkan(_window);

	// this initializes imgui for Vulkan
	ImGui_ImplVulkan_InitInfo init_info = {};
	init_info.Instance = _instance;
	init_info.PhysicalDevice = _chosenGPU;
	init_info.Device = _device;
	init_info.Queue = _graphicsQueue;
	init_info.DescriptorPool = imguiPool;
	init_info.MinImageCount = 3;
	init_info.ImageCount = 3;
	init_info.UseDynamicRendering = true;

	//dynamic rendering parameters for imgui to use
	init_info.PipelineRenderingCreateInfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	init_info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
	init_info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &_swapchainImageFormat;


	init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

	ImGui_ImplVulkan_Init(&init_info);

	ImGui_ImplVulkan_CreateFontsTexture();

	// add the destroy the imgui created structures
	_mainDeletionQueue.push_function([=]() {
		ImGui_ImplVulkan_Shutdown();
		vkDestroyDescriptorPool(_device, imguiPool, nullptr);
	});
}
void VulkanEngine::init_camera() {
	mainCamera.velocity = glm::vec3(0.f);
	mainCamera.position = glm::vec3(30.f, -00.f, -085.f);

	mainCamera.pitch = 0;
	mainCamera.yaw = 0;
}

void VulkanEngine::cleanup()
{
    if (_isInitialized) {

        vkDeviceWaitIdle(_device);
    	// make sure the gpu has stopped doing its things

    	loadedScenes.clear();

        for (int i = 0; i < FRAME_OVERLAP; i++) {

            vkDestroyCommandPool(_device, _frames[i]._commandPool, nullptr);

            //destroy sync objects
            vkDestroyFence(_device, _frames[i]._renderFence, nullptr);
            vkDestroySemaphore(_device ,_frames[i]._swapchainSemaphore, nullptr);

            _frames[i]._deletionQueue.flush();
        }
        for (int i = 0; i < _renderSemaphores.size(); i++) {
            vkDestroySemaphore(_device, _renderSemaphores[i], nullptr);
        }
        for (auto& mesh : testMeshes) {
            destroy_buffer(mesh->meshBuffers.indexBuffer);
            destroy_buffer(mesh->meshBuffers.vertexBuffer);
        }
    	assetManager.cleanup();
        _mainDeletionQueue.flush();
        destroy_swapchain();

        vkDestroySurfaceKHR(_instance, _surface, nullptr);
        vkDestroyDevice(_device, nullptr);

        vkb::destroy_debug_utils_messenger(_instance, _debug_messenger);
        vkDestroyInstance(_instance, nullptr);
        SDL_DestroyWindow(_window);
    }
}
void VulkanEngine::draw_background(VkCommandBuffer cmd)
{
    //make a clear-color from frame number. This will flash with a 120 frame period.
    VkClearColorValue clearValue;
    float flash = std::abs(std::sin(_frameNumber / 120.f));
    clearValue = { { 0.0f, 0.0f, flash, 1.0f } };

    VkImageSubresourceRange clearRange = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);

    //clear image

	ComputeEffect& effect = backgroundEffects[currentBackgroundEffect];

	// bind the background compute pipeline
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, effect.pipeline);

	// bind the descriptor set containing the draw image for the compute pipeline
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _gradientPipelineLayout, 0, 1, &_drawImageDescriptors, 0, nullptr);

	vkCmdPushConstants(cmd, _gradientPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePushConstants), &effect.data);
	// execute the compute pipeline dispatch. We are using 16x16 workgroup size so we need to divide by it
	vkCmdDispatch(cmd, std::ceil(_drawExtent.width / 16.0), std::ceil(_drawExtent.height / 16.0), 1);
}

void VulkanEngine::draw()
{
	ZoneScoped;
	if (capture_next_frame && rdoc_api) {
		rdoc_api->StartFrameCapture(NULL, NULL);
	}


	update_scene();

    VK_CHECK(vkWaitForFences(_device, 1, &get_current_frame()._renderFence, true, 1000000000));

    get_current_frame()._deletionQueue.flush();
    // get_current_frame()._frameDescriptorsAllocator.clear_pools(_device);


    _drawExtent.height = std::min(_swapchainExtent.height, _drawImage.imageExtent.height) * renderScale;
    _drawExtent.width= std::min(_swapchainExtent.width, _drawImage.imageExtent.width) * renderScale;


    //request image from the swapchain
    uint32_t swapchainImageIndex;

    VkResult e = vkAcquireNextImageKHR(_device, _swapchain, 1000000000, get_current_frame()._swapchainSemaphore, nullptr, &swapchainImageIndex);
    if (e == VK_ERROR_OUT_OF_DATE_KHR) {
        resize_requested = true;
        return ;
    }

    VK_CHECK(vkResetFences(_device, 1, &get_current_frame()._renderFence));

    VkCommandBuffer cmd = get_current_frame()._mainCommandBuffer;

    // now that we are sure that the commands finished executing, we can safely
    // reset the command buffer to begin recording again.
    VK_CHECK(vkResetCommandBuffer(cmd, 0));

    //begin the command buffer recording. We will use this command buffer exactly once, so we want to let vulkan know that
    VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    //start the command buffer recording
    // _drawExtent.width = _drawImage.imageExtent.width;
    // _drawExtent.height = _drawImage.imageExtent.height;
	auto start = std::chrono::system_clock::now();
    VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

	// transition our main draw image into general layout so we can write into it
	// we will overwrite it all so we dont care about what was the older layout
	vkutil::transition_image(cmd, _drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

	draw_background(cmd);


    vkutil::transition_image(cmd, _drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vkutil::transition_image(cmd, _depthImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    
    // Transition G-Buffer images to be used as color attachments
    vkutil::transition_image(cmd, _gAlbedo.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vkutil::transition_image(cmd, _gNormal.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vkutil::transition_image(cmd, _gORM.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);



	geometry_pass(cmd);
	auto end = std::chrono::system_clock::now();
	auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

	stats.mesh_draw_time = elapsed.count() / 1000.f;


	vkutil::transition_image(cmd, _gAlbedo.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vkutil::transition_image(cmd, _gNormal.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vkutil::transition_image(cmd, _gORM.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vkutil::transition_image(cmd, _depthImage.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);


	lighting_pass(cmd,_drawImage.imageView);

	//transtion the draw image and the swapchain image into their correct transfer layouts
	vkutil::transition_image(cmd, _drawImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	vkutil::transition_image(cmd, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // execute a copy from the draw image into the swapchain
    vkutil::copy_image_to_image(cmd, _drawImage.image, _swapchainImages[swapchainImageIndex], _drawExtent, _swapchainExtent);
    vkutil::transition_image(cmd, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    draw_imgui(cmd,  _swapchainImageViews[swapchainImageIndex]);

    // set swapchain image layout to Present so we can show it on the screen
    vkutil::transition_image(cmd, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    //finalize the command buffer (we can no longer add commands, but it can now be executed)
    VK_CHECK(vkEndCommandBuffer(cmd));

    //prepare the submission to the queue.
    //we want to wait on the _presentSemaphore, as that semaphore is signaled when the swapchain is ready
    //we will signal the _renderSemaphore, to signal that rendering has finished

    VkCommandBufferSubmitInfo cmdinfo = vkinit::command_buffer_submit_info(cmd);

    VkSemaphoreSubmitInfo waitInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR | VK_PIPELINE_STAGE_2_TRANSFER_BIT,get_current_frame()._swapchainSemaphore);
    VkSemaphoreSubmitInfo signalInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, _renderSemaphores[swapchainImageIndex]);

    VkSubmitInfo2 submit = vkinit::submit_info(&cmdinfo,&signalInfo,&waitInfo);

    //submit command buffer to the queue and execute it.
    // _renderFence will now block until the graphic commands finish execution
    VK_CHECK(vkQueueSubmit2(_graphicsQueue, 1, &submit, get_current_frame()._renderFence));

    //prepare present
    // this will put the image we just rendered to into the visible window.
    // we want to wait on the _renderSemaphore for that,
    // as its necessary that drawing commands have finished before the image is displayed to the user
    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.pNext = nullptr;
    presentInfo.pSwapchains = &_swapchain;
    presentInfo.swapchainCount = 1;

    presentInfo.pWaitSemaphores = &_renderSemaphores[swapchainImageIndex];
    presentInfo.waitSemaphoreCount = 1;

    presentInfo.pImageIndices = &swapchainImageIndex;

    VkResult presentResult = vkQueuePresentKHR(_graphicsQueue, &presentInfo);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR) {
        resize_requested = true;
    }

	if (capture_next_frame && rdoc_api) {
		rdoc_api->EndFrameCapture(NULL, NULL);
		capture_next_frame = false;
	}

    //increase the number of frames drawn
    _frameNumber++;

	FrameMark;
}


void VulkanEngine::run()
{
    SDL_Event e;
    bool bQuit = false;
	uint64_t lastTime = SDL_GetPerformanceCounter();
	uint64_t frequency = SDL_GetPerformanceFrequency();
    // main loop
    while (!bQuit) {
		ZoneScopedN("Main Loop");
        //Handle events on queue
    	uint64_t currentTime = SDL_GetPerformanceCounter();
    	// 得到的 deltaTime 单位是秒 (e.g., 0.0166s 对于 60fps)
    	float deltaTime = (float)(currentTime - lastTime) / (float)frequency;
    	// fmt::print("deltaTime: {}\n", deltaTime);
    	lastTime = currentTime;

        while (SDL_PollEvent(&e) ) {
        	auto start = std::chrono::system_clock::now();
            //close the window when user alt-f4s or clicks the X button
            if (e.type == SDL_QUIT) bQuit = true;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F11) {
                capture_next_frame = true;
            }

        	mainCamera.processSDLEvent(e);
        	ImGui_ImplSDL2_ProcessEvent(&e);

            if (e.type == SDL_WINDOWEVENT) {

                if (e.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                    stop_rendering = true;
                }
                if (e.window.event == SDL_WINDOWEVENT_RESTORED) {
                    stop_rendering = false;
                }
            }

            //send SDL event to imgui for handling
            ImGui_ImplSDL2_ProcessEvent(&e);

        	//get clock again, compare with start clock
        	auto end = std::chrono::system_clock::now();

        	//convert to microseconds (integer), and then come back to miliseconds
        	auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        	stats.frametime = elapsed.count() / 1000.f;
        }

        // 每帧只更新一次相机（放在事件循环之外）
        mainCamera.update(deltaTime);

        //do not draw if we are minimized
        if (stop_rendering) {
            //throttle the speed to avoid the endless spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if (resize_requested) {
            resize_swapchain();
        }
        // imgui new frame
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

    	ImGui::Begin("Stats");

    	ImGui::Text("frametime %f ms", stats.frametime);
    	ImGui::Text("draw time %f ms", stats.mesh_draw_time);
    	ImGui::Text("update time %f ms", stats.scene_update_time);
    	ImGui::Text("triangles %i", stats.triangle_count);
    	ImGui::Text("draws %i", stats.drawcall_count);
		if (ImGui::Button("Capture Frame (RenderDoc)")) {
			capture_next_frame = true;
		}
    	ImGui::End();

        if (ImGui::Begin("background")) {

            ImGui::SliderFloat("Render Scale",&renderScale, 0.3f, 1.f);

            ComputeEffect& selected = backgroundEffects[currentBackgroundEffect];

            ImGui::Text("Selected effect: ", selected.name);

            ImGui::SliderInt("Effect Index", &currentBackgroundEffect,0, backgroundEffects.size() - 1);

            ImGui::InputFloat4("data1",(float*)& selected.data.data1);
            ImGui::InputFloat4("data2",(float*)& selected.data.data2);
            ImGui::InputFloat4("data3",(float*)& selected.data.data3);
            ImGui::InputFloat4("data4",(float*)& selected.data.data4);
        }
        ImGui::End();

        ImGui::Render();


        //our draw function
        draw();


    }
}
void VulkanEngine::init_vulkan()
{
    vkb::InstanceBuilder builder;

    //make the vulkan instance, with basic debug features
    auto inst_ret = builder.set_app_name("Example Vulkan Application")
        .request_validation_layers(bUseValidationLayers)
        .use_default_debug_messenger()
        .require_api_version(1, 3, 0)
        .build();

    vkb::Instance vkb_inst = inst_ret.value();

    //grab the instance
    _instance = vkb_inst.instance;
    _debug_messenger = vkb_inst.debug_messenger;

    SDL_Vulkan_CreateSurface(_window, _instance, &_surface);

    //vulkan 1.3 features
    VkPhysicalDeviceVulkan13Features features{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    features.dynamicRendering = true;
    features.synchronization2 = true;

    //vulkan 1.2 features
    VkPhysicalDeviceVulkan12Features features12{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    features12.bufferDeviceAddress = true;

    features12.descriptorIndexing = true;
	features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
	features12.descriptorBindingPartiallyBound           = VK_TRUE;
	features12.descriptorBindingVariableDescriptorCount  = VK_TRUE;
	features12.runtimeDescriptorArray                   = VK_TRUE;

	features12.descriptorIndexing = VK_TRUE;



    //use vkbootstrap to select a gpu.
    //We want a gpu that can write to the SDL surface and supports vulkan 1.3 with the correct features
    vkb::PhysicalDeviceSelector selector{ vkb_inst };
    vkb::PhysicalDevice physicalDevice = selector
        .set_minimum_version(1, 3)
        .set_required_features_13(features)
        .set_required_features_12(features12)
        .set_surface(_surface)
        .select()
        .value();


    //create the final vulkan device
    vkb::DeviceBuilder deviceBuilder{ physicalDevice };

    vkb::Device vkbDevice = deviceBuilder.build().value();

    // Get the VkDevice handle used in the rest of a vulkan application
    _device = vkbDevice.device;
    _chosenGPU = physicalDevice.physical_device;


    // use vkbootstrap to get a Graphics queue
    _graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
    _graphicsQueueFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

    // initialize the memory allocator
    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.physicalDevice = _chosenGPU;
    allocatorInfo.device = _device;
    allocatorInfo.instance = _instance;
    allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    vmaCreateAllocator(&allocatorInfo, &_allocator);

    _mainDeletionQueue.push_function([&]() {
 //    		char* statsString;
	// vmaBuildStatsString(_allocator, &statsString, VK_TRUE);
	//
	// // 把这个字符串打印到控制台，或者保存成 JSON 文件
 //    	fmt::print("VMA Unfreed Resources:\n{}\n", statsString);
	//
	// // 记得释放字符串内存
	// vmaFreeStatsString(_allocator, statsString);

        vmaDestroyAllocator(_allocator);
    });


}
void VulkanEngine::draw_imgui(VkCommandBuffer cmd, VkImageView targetImageView)
{
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(targetImageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingInfo renderInfo = vkinit::rendering_info(_swapchainExtent, &colorAttachment, nullptr);

    vkCmdBeginRendering(cmd, &renderInfo);

    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

    vkCmdEndRendering(cmd);
}
void VulkanEngine::init_commands()
{
    // //create a command pool for commands submitted to the graphics queue.
    // //we also want the pool to allow for resetting of individual command buffers
    // VkCommandPoolCreateInfo commandPoolInfo =  {};
    // commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    // commandPoolInfo.pNext = nullptr;
    // commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    // commandPoolInfo.queueFamilyIndex = _graphicsQueueFamily;
    //
    // for (int i = 0; i < FRAME_OVERLAP; i++) {
    //
    //     VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr, &_frames[i]._commandPool));
    //
    //     // allocate the default command buffer that we will use for rendering
    //     VkCommandBufferAllocateInfo cmdAllocInfo = {};
    //     cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    //     cmdAllocInfo.pNext = nullptr;
    //     cmdAllocInfo.commandPool = _frames[i]._commandPool;
    //     cmdAllocInfo.commandBufferCount = 1;
    //     cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    //
    //     VK_CHECK(vkAllocateCommandBuffers(_device, &cmdAllocInfo, &_frames[i]._mainCommandBuffer));
    // }

    VkCommandPoolCreateInfo commandPoolInfo = vkinit::command_pool_create_info(_graphicsQueueFamily, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);

    for (int i = 0; i < FRAME_OVERLAP; i++) {

        VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr, &_frames[i]._commandPool));

        // allocate the default command buffer that we will use for rendering
        VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(_frames[i]._commandPool, 1);

        VK_CHECK(vkAllocateCommandBuffers(_device, &cmdAllocInfo, &_frames[i]._mainCommandBuffer));
    }

    VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr, &_immCommandPool));

    // allocate the command buffer for immediate submits
    VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(_immCommandPool, 1);

    VK_CHECK(vkAllocateCommandBuffers(_device, &cmdAllocInfo, &_immCommandBuffer));

    _mainDeletionQueue.push_function([=]() {
    vkDestroyCommandPool(_device, _immCommandPool, nullptr);
    });

}
void VulkanEngine::init_sync_structures()
{
    VkFenceCreateInfo fenceCreateInfo = vkinit::fence_create_info(VK_FENCE_CREATE_SIGNALED_BIT);
    VkSemaphoreCreateInfo semaphoreCreateInfo = vkinit::semaphore_create_info();

    int swapchainImageCount = _swapchainImages.size();
    _renderSemaphores.resize(swapchainImageCount);
    VkSemaphoreCreateInfo renderSemaphoreCreateInfo = vkinit::semaphore_create_info();

    for (int i = 0; i < swapchainImageCount; i++) {
        VK_CHECK(vkCreateSemaphore(_device, &renderSemaphoreCreateInfo, nullptr, &_renderSemaphores[i]));
    }

    VK_CHECK(vkCreateFence(_device, &fenceCreateInfo, nullptr, &_immFence));
    _mainDeletionQueue.push_function([=]() { vkDestroyFence(_device, _immFence, nullptr); });

    for (int i=0;i<FRAME_OVERLAP;i++)
    {
        VK_CHECK(vkCreateFence(_device,&fenceCreateInfo,nullptr,&_frames[i]._renderFence));
        VK_CHECK(vkCreateSemaphore(_device,&semaphoreCreateInfo,nullptr,&_frames[i]._swapchainSemaphore));

    }
}

// 立即提交命令到 GPU 队列执行，并同步等待执行完成
// 用于初始化、数据上传等需要在 CPU 端立即知道 GPU 执行状态的操作
void VulkanEngine::immediate_submit(std::function<void(VkCommandBuffer cmd)>&& function)
{
    VK_CHECK(vkResetFences(_device, 1, &_immFence));
    VK_CHECK(vkResetCommandBuffer(_immCommandBuffer, 0));

    VkCommandBuffer cmd = _immCommandBuffer;

    VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

    function(cmd);

    VK_CHECK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo cmdinfo = vkinit::command_buffer_submit_info(cmd);
    VkSubmitInfo2 submit = vkinit::submit_info(&cmdinfo, nullptr, nullptr);

    // submit command buffer to the queue and execute it.
    //  _renderFence will now block until the graphic commands finish execution
    VK_CHECK(vkQueueSubmit2(_graphicsQueue, 1, &submit, _immFence));

    VK_CHECK(vkWaitForFences(_device, 1, &_immFence, true, 9999999999));
}

void VulkanEngine::init_descriptors() {
    //create a descriptor pool that will hold 10 sets with 1 image each
    std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> sizes =
    {
    	{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
	    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},        // 对应 binding 0 (材质参数)
	    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5} // 对应 binding 1-4 (4张贴图)
    };

    globalDescriptorAllocator.init(_device, 10, sizes);

    //make the descriptor set layout for our compute draw
    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        _drawImageDescriptorLayout = builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
    }

    _drawImageDescriptors = globalDescriptorAllocator.allocate(_device,_drawImageDescriptorLayout);


    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);   // Camera/Scene UBO
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);   // Object SSBO
        _gpuSceneDataDescriptorLayout = builder.build(_device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    }


	_mainDeletionQueue.push_function([=, this]() {
	vkDestroyDescriptorSetLayout(_device, _gpuSceneDataDescriptorLayout, nullptr);
});

    DescriptorWriter writer;
    writer.write_image(0, _drawImage.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);

    writer.update_set(_device,_drawImageDescriptors);

    //make sure both the descriptor allocator and the new layout get cleaned up properly
    _mainDeletionQueue.push_function([&]() {
        globalDescriptorAllocator.destroy_pools(_device);

        vkDestroyDescriptorSetLayout(_device, _drawImageDescriptorLayout, nullptr);
    });



    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0); // Material Buffer
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT,
            2000); // 可变贴图数组，必须与 variableCount 和 pool 大小一致
        _bindlessDescriptorLayout = builder.build(
            _device,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            nullptr,
            VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT); // pool 有这个 flag，layout 必须匹配
    }
    
    uint32_t variableCount = 2000;
    VkDescriptorSetVariableDescriptorCountAllocateInfo variableAllocInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
        .descriptorSetCount = 1,
        .pDescriptorCounts = &variableCount
    };
    
    // Create a dedicated descriptor pool for bindless
    VkDescriptorPoolSize bindless_pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4000 }
    };
    VkDescriptorPoolCreateInfo bindless_pool_info = {};
    bindless_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    bindless_pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    bindless_pool_info.maxSets = 1;
    bindless_pool_info.poolSizeCount = 2;
    bindless_pool_info.pPoolSizes = bindless_pool_sizes;

    VkDescriptorPool bindlessPool;
    VK_CHECK(vkCreateDescriptorPool(_device, &bindless_pool_info, nullptr, &bindlessPool));

    VkDescriptorSetAllocateInfo bindlessAllocInfo = {};
    bindlessAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    bindlessAllocInfo.pNext = &variableAllocInfo;
    bindlessAllocInfo.descriptorPool = bindlessPool;
    bindlessAllocInfo.descriptorSetCount = 1;
    bindlessAllocInfo.pSetLayouts = &_bindlessDescriptorLayout;

    VK_CHECK(vkAllocateDescriptorSets(_device, &bindlessAllocInfo, &_bindlessDescriptorSet));

    _mainDeletionQueue.push_function([this, bindlessPool]() {
        vkDestroyDescriptorPool(_device, bindlessPool, nullptr);
    });

    // Initial Material Buffer sizing for 2000 materials
    _materialBuffer = create_buffer(sizeof(GLTFMetallic_Roughness::MaterialConstants) * 4000,
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
                                    VMA_MEMORY_USAGE_CPU_TO_GPU);

    DescriptorWriter bindlessWriter;
    bindlessWriter.write_buffer(0, _materialBuffer.buffer, sizeof(GLTFMetallic_Roughness::MaterialConstants) * 4000, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    bindlessWriter.update_set(_device, _bindlessDescriptorSet);

    _mainDeletionQueue.push_function([this]() {
        vkDestroyDescriptorSetLayout(_device, _bindlessDescriptorLayout, nullptr);
        destroy_buffer(_materialBuffer);
    });

    for (int i = 0; i < FRAME_OVERLAP; i++) {
        // create a descriptor pool
        std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> frame_sizes = {
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 },
        };

        _frames[i]._frameDescriptorsAllocator = DescriptorAllocatorGrowable{};
        _frames[i]._frameDescriptorsAllocator.init(_device, 1000, frame_sizes);

        _mainDeletionQueue.push_function([&, i]() {
            _frames[i]._frameDescriptorsAllocator.destroy_pools(_device);
        });

    	_frames[i].objectStorageBuffer = create_buffer(
		sizeof(GPUObjectData) * MAX_OBJECTS,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		VMA_MEMORY_USAGE_CPU_TO_GPU
	);

    	// 2. 创建这一帧的摄像机 UBO
    	_frames[i].cameraBuffer = create_buffer(
			sizeof(GPUSceneData),
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VMA_MEMORY_USAGE_CPU_TO_GPU
		);

    	// 3. 从这一帧的池子里分配 Set 0
    	_frames[i].globalDescriptor = _frames[i]._frameDescriptorsAllocator.allocate(_device, _gpuSceneDataDescriptorLayout);

    	// 4. 更新 Descriptor Set (把 Buffer 指针填进去)
    	DescriptorWriter writer;

    	// Binding 0: Camera UBO
    	writer.write_buffer(0, _frames[i].cameraBuffer.buffer, sizeof(GPUSceneData), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);

    	// Binding 1: Object SSBO
    	writer.write_buffer(1, _frames[i].objectStorageBuffer.buffer, sizeof(GPUObjectData) * MAX_OBJECTS, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    	// 提交更新
    	writer.update_set(_device, _frames[i].globalDescriptor);

    	// 5. 注册销毁 (注意用 [=, this])
    	_mainDeletionQueue.push_function([=, this]() {
			destroy_buffer(_frames[i].objectStorageBuffer);
			destroy_buffer(_frames[i].cameraBuffer);
			// descriptor set 会随着 pool 自动销毁，不用管
		});


    }



}

void VulkanEngine::init_pipelines()
{
    //COMPUTE PIPELINES
    init_background_pipelines();

    // GRAPHICS PIPELINES
    metalRoughMaterial.build_pipelines(this);
    init_Deferredlighting_pipeline();

    // Build the default (fallback) material now that materialLayout is ready.
    // This MUST happen after build_pipelines() which creates materialLayout.
    GLTFMetallic_Roughness::MaterialResources materialResources;
    materialResources.colorImage        = _whiteImage;
    materialResources.colorSampler      = _defaultSamplerLinear;
    materialResources.metalRoughImage   = _greyImage;
    materialResources.metalRoughSampler = _defaultSamplerLinear;
    materialResources.normalImage       = _defaultNormalImage;
    materialResources.normalSampler     = _defaultSamplerLinear;
    materialResources.occlusionImage    = _whiteImage;
    materialResources.occlusionSampler  = _defaultSamplerLinear;
    materialResources.emissiveImage     = _blackImage;
    materialResources.emissiveSampler   = _defaultSamplerLinear;

    GLTFMetallic_Roughness::MaterialConstants constants{};
    constants.colorFactors        = glm::vec4{1, 1, 1, 1};
    constants.metal_rough_factors = glm::vec4{0, 0.5f, 0, 0};
    constants.emissive_factors    = glm::vec4{0, 0, 0, 0};

    materialResources.data = constants;

    defaultData = metalRoughMaterial.write_material(_device, MaterialPass::MainColor,
        materialResources, globalDescriptorAllocator);
}

// 将网格的顶点数据和索引数据上传到 GPU
// 内部使用了一个 staging buffer (暂存区缓冲区) 来暂存 CPU 数据，并通过 immediate_submit 拷贝到最终的 GPU buffer 中
GPUMeshBuffers VulkanEngine::uploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices)
{
    const size_t vertexBufferSize = vertices.size() * sizeof(Vertex);
    const size_t indexBufferSize = indices.size() * sizeof(uint32_t);

    GPUMeshBuffers newSurface;

    //create vertex buffer
    newSurface.vertexBuffer = create_buffer(vertexBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    //find the adress of the vertex buffer
    VkBufferDeviceAddressInfo deviceAdressInfo{ .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,.buffer = newSurface.vertexBuffer.buffer };
    newSurface.vertexBufferAddress = vkGetBufferDeviceAddress(_device, &deviceAdressInfo);

    //create index buffer
    newSurface.indexBuffer = create_buffer(indexBufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);
    AllocatedBuffer staging = create_buffer(vertexBufferSize + indexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);

    void* data = staging.allocation->GetMappedData();

    // copy vertex buffer
    memcpy(data, vertices.data(), vertexBufferSize);
    // copy index buffer
    memcpy((char*)data + vertexBufferSize, indices.data(), indexBufferSize);

    immediate_submit([&](VkCommandBuffer cmd) {
        VkBufferCopy vertexCopy{ 0 };
        vertexCopy.dstOffset = 0;
        vertexCopy.srcOffset = 0;
        vertexCopy.size = vertexBufferSize;

        vkCmdCopyBuffer(cmd, staging.buffer, newSurface.vertexBuffer.buffer, 1, &vertexCopy);

        VkBufferCopy indexCopy{ 0 };
        indexCopy.dstOffset = 0;
        indexCopy.srcOffset = vertexBufferSize;
        indexCopy.size = indexBufferSize;

        vkCmdCopyBuffer(cmd, staging.buffer, newSurface.indexBuffer.buffer, 1, &indexCopy);
    });

    destroy_buffer(staging);

    return newSurface;
}

// 使用 VMA (Vulkan Memory Allocator) 创建一个 Buffer (缓冲区对象)
// 支持配置 Buffer 的用途 (usage) 和内存分布策略 (memoryUsage，如在单独显存，或者 CPU 可见)
AllocatedBuffer VulkanEngine::create_buffer(size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage)
{
    // allocate buffer
    VkBufferCreateInfo bufferInfo = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.pNext = nullptr;
    bufferInfo.size = allocSize;

    bufferInfo.usage = usage;

    VmaAllocationCreateInfo vmaallocInfo = {};
    vmaallocInfo.usage = memoryUsage;
    vmaallocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    AllocatedBuffer newBuffer;

    // allocate the buffer
    VK_CHECK(vmaCreateBuffer(_allocator, &bufferInfo, &vmaallocInfo, &newBuffer.buffer, &newBuffer.allocation,
        &newBuffer.info));

    return newBuffer;
}

// 释放指定的 Buffer 及其分配的显存/内存资源
void VulkanEngine::destroy_buffer(const AllocatedBuffer& buffer)
{
    vmaDestroyBuffer(_allocator, buffer.buffer, buffer.allocation);
}


// 在 GPU 上分配并创建一个指定大小、格式和用途的 Image (图像/纹理) 以及它的 ImageView
// 默认分配在 Device Local (GPU 专用) 显存中，可选是否开启 Mipmap (多级缓存) 支持
AllocatedImage VulkanEngine::create_image(VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped)
{
    AllocatedImage newImage;
    newImage.imageFormat = format;
    newImage.imageExtent = size;

    VkImageCreateInfo img_info = vkinit::image_create_info(format, usage, size);
    if (mipmapped) {
        img_info.mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(size.width, size.height)))) + 1;
    }

    // always allocate images on dedicated GPU memory
    VmaAllocationCreateInfo allocinfo = {};
    allocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    allocinfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // allocate and create the image
    VK_CHECK(vmaCreateImage(_allocator, &img_info, &allocinfo, &newImage.image, &newImage.allocation, nullptr));

    // if the format is a depth format, we will need to have it use the correct
    // aspect flag
    VkImageAspectFlags aspectFlag = VK_IMAGE_ASPECT_COLOR_BIT;
    if (format == VK_FORMAT_D32_SFLOAT) {
        aspectFlag = VK_IMAGE_ASPECT_DEPTH_BIT;
    }

    // build a image-view for the image
    VkImageViewCreateInfo view_info = vkinit::imageview_create_info(format, newImage.image, aspectFlag);
    view_info.subresourceRange.levelCount = img_info.mipLevels;

    VK_CHECK(vkCreateImageView(_device, &view_info, nullptr, &newImage.imageView));

    return newImage;
}
// 创建一个 Image，并将给定的 CPU 端数据作为初始内容，使用 staging buffer 上传到这个 Image 中
// 如果需要，会在数据上传完毕后调用 vkutil::generate_mipmaps 自动生成后续的所有 Mipmap 级别
AllocatedImage VulkanEngine::create_image(void* data, VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped)
{
	size_t data_size = size.depth * size.width * size.height * 4;
	AllocatedBuffer uploadbuffer = create_buffer(data_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	memcpy(uploadbuffer.info.pMappedData, data, data_size);

	AllocatedImage new_image = create_image(size, format, usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, mipmapped);

	immediate_submit([&](VkCommandBuffer cmd) {
		vkutil::transition_image(cmd, new_image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		VkBufferImageCopy copyRegion = {};
		copyRegion.bufferOffset = 0;
		copyRegion.bufferRowLength = 0;
		copyRegion.bufferImageHeight = 0;

		copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.imageSubresource.mipLevel = 0;
		copyRegion.imageSubresource.baseArrayLayer = 0;
		copyRegion.imageSubresource.layerCount = 1;
		copyRegion.imageExtent = size;

		// copy the buffer into the image
		vkCmdCopyBufferToImage(cmd, uploadbuffer.buffer, new_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
			&copyRegion);

		if (mipmapped) {
			vkutil::generate_mipmaps(cmd, new_image.image,VkExtent2D{new_image.imageExtent.width,new_image.imageExtent.height});
		} else {
			vkutil::transition_image(cmd, new_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}
	});
	destroy_buffer(uploadbuffer);
	return new_image;
}

// 销毁指定的 Image 及其绑定的 ImageView，同时释放所占据的显存资源
void VulkanEngine::destroy_image(const AllocatedImage& img)
{
    vkDestroyImageView(_device, img.imageView, nullptr);
    vmaDestroyImage(_allocator, img.image, img.allocation);
}

bool is_visible(const RenderObject& obj, const glm::mat4& viewproj) {
	std::array<glm::vec3, 8> corners {
		glm::vec3 { 1, 1, 1 },
		glm::vec3 { 1, 1, -1 },
		glm::vec3 { 1, -1, 1 },
		glm::vec3 { 1, -1, -1 },
		glm::vec3 { -1, 1, 1 },
		glm::vec3 { -1, 1, -1 },
		glm::vec3 { -1, -1, 1 },
		glm::vec3 { -1, -1, -1 },
	};

	glm::mat4 matrix = viewproj * obj.transform;

	glm::vec3 min = { 1.5, 1.5, 1.5 };
	glm::vec3 max = { -1.5, -1.5, -1.5 };

	for (int c = 0; c < 8; c++) {
		// project each corner into clip space
		glm::vec4 v = matrix * glm::vec4(obj.bounds.origin + (corners[c] * obj.bounds.extents), 1.f);

		// perspective correction
		v.x = v.x / v.w;
		v.y = v.y / v.w;
		v.z = v.z / v.w;

		min = glm::min(glm::vec3 { v.x, v.y, v.z }, min);
		max = glm::max(glm::vec3 { v.x, v.y, v.z }, max);
	}

	// check the clip space box is within the view
	if (min.z > 1.f || max.z < 0.f || min.x > 1.f || max.x < -1.f || min.y > 1.f || max.y < -1.f) {
		return false;
	} else {
		return true;
	}
}

void VulkanEngine::geometry_pass(VkCommandBuffer cmd)
{
	ZoneScoped;

	std::vector<uint32_t> opaque_draws;
	opaque_draws.reserve(mainDrawContext.OpaqueSurfaces.size());

	for (uint32_t i = 0; i < mainDrawContext.OpaqueSurfaces.size(); i++) {
		if (is_visible(mainDrawContext.OpaqueSurfaces[i], sceneData.viewproj)) {
			opaque_draws.push_back(i);
		}
		// opaque_draws.push_back(i);
	}

	// sort the opaque surfaces by material and mesh
	std::sort(opaque_draws.begin(), opaque_draws.end(), [&](const auto& iA, const auto& iB) {
		const RenderObject& A = mainDrawContext.OpaqueSurfaces[iA];
		const RenderObject& B = mainDrawContext.OpaqueSurfaces[iB];
		if (A.material == B.material) {
			return A.indexBuffer < B.indexBuffer;
		}
		else {
			return A.material < B.material;
		}
	});
	//reset counters
	stats.drawcall_count = 0;
	stats.triangle_count = 0;

	VkClearValue clearColor;
	clearColor.color = { { 0.0f, 0.0f, 0.0f, 0.0f } };

	//begin a render pass  connected to our draw image
	VkRenderingAttachmentInfo albedoAttachment = vkinit::attachment_info(
			_gAlbedo.imageView, &clearColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo normalAttachment = vkinit::attachment_info(
		_gNormal.imageView, &clearColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo ormAttachment = vkinit::attachment_info(
		_gORM.imageView, &clearColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
			_depthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo colorAttachments[] = { albedoAttachment, normalAttachment, ormAttachment };
	// VkRenderingInfo renderInfo = vkinit::rendering_info(_drawExtent,colorAttachments, depthAttachment);
	// VkRenderingInfo renderInfo = vkinit::rendering_info(_drawExtent, colorAttachments, depthAttachment);
	// renderInfo.colorAttachmentCount = 3;
	VkRenderingInfo renderInfo = {};
	renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
	renderInfo.renderArea = VkRect2D{ {0, 0}, _drawExtent };
	renderInfo.layerCount = 1;
	renderInfo.colorAttachmentCount = 3;
	renderInfo.pColorAttachments = colorAttachments;
	renderInfo.pDepthAttachment = &depthAttachment;

	vkCmdBeginRendering(cmd, &renderInfo);



	//set dynamic viewport and scissor
	VkViewport viewport = {};
	viewport.x = 0;
	viewport.y = 0;
	viewport.width = _drawExtent.width;
	viewport.height = _drawExtent.height;
	viewport.minDepth = 0.f;
	viewport.maxDepth = 1.f;

	vkCmdSetViewport(cmd, 0, 1, &viewport);

	VkRect2D scissor = {};
	scissor.offset.x = 0;
	scissor.offset.y = 0;
	scissor.extent.width = viewport.width;
	scissor.extent.height = viewport.height;

	vkCmdSetScissor(cmd, 0, 1, &scissor);


	// 使用持久化的 FrameData buffer，直接写入新数据（buffer 和 descriptor 在 init_descriptors 已绑定好）
	FrameData& currentFrame = get_current_frame();
	GPUSceneData* sceneUniformData = (GPUSceneData*)currentFrame.cameraBuffer.allocation->GetMappedData();
	*sceneUniformData = sceneData;

	// 直接取现成的 globalDescriptor，无需重新 allocate 或 update_set
	VkDescriptorSet globalDescriptor = currentFrame.globalDescriptor;

    MaterialPipeline* lastPipeline = nullptr;
    MaterialInstance* lastMaterial = nullptr;
    VkBuffer lastIndexBuffer = VK_NULL_HANDLE;

    auto draw = [&](const RenderObject& r) {
        if (r.material->pipeline != lastPipeline) {
            lastPipeline = r.material->pipeline;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.material->pipeline->pipeline);
            
            // Bind both Set 0 (Per frame Camera/Object Data) and Set 1 (Bindless Asset Data)
            VkDescriptorSet descriptorSets[] = { globalDescriptor, _bindlessDescriptorSet };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.material->pipeline->layout, 0, 2, descriptorSets, 0, nullptr);
        }
        
        if (r.indexBuffer != lastIndexBuffer) {
            lastIndexBuffer = r.indexBuffer;
            vkCmdBindIndexBuffer(cmd, r.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        }
        
        // calculate final mesh matrix
        GPUDrawPushConstants push_constants;
        push_constants.worldMatrix = r.transform;
        push_constants.vertexBuffer = r.vertexBufferAddress;
        push_constants.materialID = r.material->materialID;

        vkCmdPushConstants(cmd, r.material->pipeline->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GPUDrawPushConstants), &push_constants);

        stats.drawcall_count++;
        stats.triangle_count += r.indexCount / 3;
        vkCmdDrawIndexed(cmd, r.indexCount, 1, r.firstIndex, 0, 0);
    };


	for (auto& r : opaque_draws) {
		draw(mainDrawContext.OpaqueSurfaces[r]);
	}
	for (auto& r : mainDrawContext.TransparentSurfaces) {
		draw(r);
	}
	mainDrawContext.OpaqueSurfaces.clear();
	mainDrawContext.TransparentSurfaces.clear();

	vkCmdEndRendering(cmd);



}
void VulkanEngine::lighting_pass(VkCommandBuffer cmd, VkImageView targetImageView)
{
	ZoneScoped;

	VkRenderingAttachmentInfo swapchainAttachment = vkinit::attachment_info(targetImageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingInfo renderInfo = {};
	renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
	renderInfo.renderArea = VkRect2D{ {0, 0}, _drawExtent };
	renderInfo.layerCount = 1;
	renderInfo.colorAttachmentCount = 1;
	renderInfo.pColorAttachments = &swapchainAttachment;

	vkCmdBeginRendering(cmd, &renderInfo);



	//set dynamic viewport and scissor
	VkViewport viewport = {};
	viewport.x = 0;
	viewport.y = 0;
	viewport.width = _drawExtent.width;
	viewport.height = _drawExtent.height;
	viewport.minDepth = 0.f;
	viewport.maxDepth = 1.f;

	vkCmdSetViewport(cmd, 0, 1, &viewport);

	VkRect2D scissor = {};
	scissor.offset.x = 0;
	scissor.offset.y = 0;
	scissor.extent.width = viewport.width;
	scissor.extent.height = viewport.height;

	vkCmdSetScissor(cmd, 0, 1, &scissor);



	vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,_deferredLightingPipeline);
	//vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _deferredLightingPipelineLayout, 0, 1, &_gBufferDescriptorSet, 0, nullptr);
	FrameData& currentFrame = get_current_frame();
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _deferredLightingPipelineLayout,
							0, 1, &currentFrame.globalDescriptor, 0, nullptr); // 绑定 Set 0 (SceneData)
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _deferredLightingPipelineLayout,
							1, 1, &_gBufferDescriptorSet, 0, nullptr);         // 绑定 Set 1 (G-Buffer)
	// 绘制 3 个没有 Vertex Buffer 指派的顶点
	vkCmdDraw(cmd, 3, 1, 0, 0);
	//
	vkCmdEndRendering(cmd);


}
void VulkanEngine::init_background_pipelines()
{
    VkPipelineLayoutCreateInfo computeLayout{};
    computeLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    computeLayout.pNext = nullptr;
    computeLayout.pSetLayouts = &_drawImageDescriptorLayout;
    computeLayout.setLayoutCount = 1;

    VkPushConstantRange pushConstant{};
    pushConstant.offset = 0;
    pushConstant.size = sizeof(ComputePushConstants) ;
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    computeLayout.pPushConstantRanges = &pushConstant;
    computeLayout.pushConstantRangeCount = 1;

    VK_CHECK(vkCreatePipelineLayout(_device, &computeLayout, nullptr, &_gradientPipelineLayout));


    VkShaderModule gradientShader;
    if (!vkutil::load_shader_module("../cmake-build-debug-mingw/shaders/gradient.comp.spv", _device, &gradientShader)) {
	    fmt::print("Error when building the compute shader \n");
    }

    VkShaderModule skyShader;
    if (!vkutil::load_shader_module("../cmake-build-debug-mingw/shaders/sky.comp.spv", _device, &skyShader)) {
	    fmt::print("Error when building the compute shader \n");
    }

    VkPipelineShaderStageCreateInfo stageinfo{};
    stageinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageinfo.pNext = nullptr;
    stageinfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageinfo.module = gradientShader;
    stageinfo.pName = "main";

    VkComputePipelineCreateInfo computePipelineCreateInfo{};
    computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    computePipelineCreateInfo.pNext = nullptr;
    computePipelineCreateInfo.layout = _gradientPipelineLayout;
    computePipelineCreateInfo.stage = stageinfo;

    ComputeEffect gradient;
    gradient.layout = _gradientPipelineLayout;
    gradient.name = "gradient";
    gradient.data = {};

    //default colors
    gradient.data.data1 = glm::vec4(1, 0, 0, 1);
    gradient.data.data2 = glm::vec4(0, 0, 1, 1);

    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &gradient.pipeline));

    //change the shader module only to create the sky shader
    computePipelineCreateInfo.stage.module = skyShader;

    ComputeEffect sky;
    sky.layout = _gradientPipelineLayout;
    sky.name = "sky";
    sky.data = {};
    //default sky parameters
    sky.data.data1 = glm::vec4(0.1, 0.2, 0.4 ,0.97);

    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &sky.pipeline));

    //add the 2 background effects into the array
    backgroundEffects.push_back(gradient);
    backgroundEffects.push_back(sky);

    //destroy structures properly
    vkDestroyShaderModule(_device, gradientShader, nullptr);
    vkDestroyShaderModule(_device, skyShader, nullptr);
    _mainDeletionQueue.push_function([=]() {
	    vkDestroyPipelineLayout(_device, _gradientPipelineLayout, nullptr);
	    vkDestroyPipeline(_device, sky.pipeline, nullptr);
	    vkDestroyPipeline(_device, gradient.pipeline, nullptr);
    });

}
void VulkanEngine::init_Deferredlighting_pipeline()
{
	VkShaderModule lightingShader;
	if (!vkutil::load_shader_module("../cmake-build-debug-mingw/shaders/deferred_lighting.vert.spv", _device, &lightingShader)) {
		fmt::print("Error when building the deferred lighting shader module \n");
	}

	VkShaderModule deferredLightingShader;
	if (!vkutil::load_shader_module("../cmake-build-debug-mingw/shaders/deferred_lighting.frag.spv", _device, &deferredLightingShader)) {
		fmt::print("Error when building the deferred lighting shader module \n");
	}
	VkPipelineLayoutCreateInfo layoutCreateInfo = vkinit::pipeline_layout_create_info();
	layoutCreateInfo.setLayoutCount = 2;
	VkDescriptorSetLayout lightingLayouts[] = { _gpuSceneDataDescriptorLayout, _gBufferDescriptorLayout};
	layoutCreateInfo.pSetLayouts = lightingLayouts;
	layoutCreateInfo.pPushConstantRanges = nullptr;
	layoutCreateInfo.pushConstantRangeCount = 0;

	VkPipelineLayout pipelineLayout;
	VK_CHECK(vkCreatePipelineLayout(_device, &layoutCreateInfo, nullptr, &pipelineLayout));
	_deferredLightingPipelineLayout = pipelineLayout;

	PipelineBuilder pipelineBuilder;
	pipelineBuilder.set_shaders(lightingShader, deferredLightingShader);
	pipelineBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	pipelineBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	pipelineBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	pipelineBuilder.set_multisampling_none();
	pipelineBuilder.disable_blending();
	pipelineBuilder.enable_depthtest(false, VK_COMPARE_OP_ALWAYS);

	pipelineBuilder.set_color_attachment_formats({ VK_FORMAT_R16G16B16A16_SFLOAT });
	pipelineBuilder.set_depth_format(_depthImage.imageFormat);
	pipelineBuilder._pipelineLayout = pipelineLayout;

	_deferredLightingPipeline = pipelineBuilder.build_pipeline(_device);
	_mainDeletionQueue.push_function([=]() {
		vkDestroyPipelineLayout(_device, pipelineLayout, nullptr);
		vkDestroyPipeline(_device, _deferredLightingPipeline, nullptr);
	});

	vkDestroyShaderModule(_device, lightingShader, nullptr);
	vkDestroyShaderModule(_device, deferredLightingShader, nullptr);

}
void GLTFMetallic_Roughness::build_pipelines(VulkanEngine* engine)
{
	VkShaderModule meshFragShader;
	if (!vkutil::load_shader_module("../cmake-build-debug-mingw/shaders/gbuffer.frag.spv", engine->_device, &meshFragShader)) {
		fmt::println("Error when building the gbuffer fragment shader module");
	}

	VkShaderModule meshVertexShader;
	if (!vkutil::load_shader_module("../cmake-build-debug-mingw/shaders/gbuffer.vert.spv", engine->_device, &meshVertexShader)) {
		fmt::println("Error when building the gbuffer vertex shader module");
	}

	VkPushConstantRange matrixRange{};
	matrixRange.offset = 0;
	matrixRange.size = sizeof(GPUDrawPushConstants);
	matrixRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

	VkDescriptorSetLayout layouts[] = { engine->_gpuSceneDataDescriptorLayout,
        engine->_bindlessDescriptorLayout };

	VkPipelineLayoutCreateInfo mesh_layout_info = vkinit::pipeline_layout_create_info();
	mesh_layout_info.setLayoutCount = 2;
	mesh_layout_info.pSetLayouts = layouts;
	mesh_layout_info.pPushConstantRanges = &matrixRange;
	mesh_layout_info.pushConstantRangeCount = 1;

	VkPipelineLayout newLayout;
	VK_CHECK(vkCreatePipelineLayout(engine->_device, &mesh_layout_info, nullptr, &newLayout));

    opaquePipeline.layout = newLayout;
    transparentPipeline.layout = newLayout;

	// build the stage-create-info for both vertex and fragment stages. This lets
	// the pipeline know the shader modules per stage
	PipelineBuilder pipelineBuilder;
	pipelineBuilder.set_shaders(meshVertexShader, meshFragShader);
	pipelineBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	pipelineBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	pipelineBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	pipelineBuilder.set_multisampling_none();
	pipelineBuilder.disable_blending();
	pipelineBuilder.enable_depthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);

	//render formats for G-Buffer MRT
    std::vector<VkFormat> gbufferFormats = { 
        VK_FORMAT_R8G8B8A8_UNORM,       // Albedo matches _gAlbedo format
        VK_FORMAT_R16G16B16A16_SFLOAT,  // Normal matches _gNormal format
        VK_FORMAT_R8G8B8A8_UNORM        // ORM matches _gORM format
    };
	pipelineBuilder.set_color_attachment_formats(gbufferFormats);
	pipelineBuilder.set_depth_format(engine->_depthImage.imageFormat);

	// use the triangle layout we created
	pipelineBuilder._pipelineLayout = newLayout;

	// finally build the pipeline
    opaquePipeline.pipeline = pipelineBuilder.build_pipeline(engine->_device);

	// create the transparent variant
	pipelineBuilder.enable_blending_additive();

	pipelineBuilder.enable_depthtest(false, VK_COMPARE_OP_GREATER_OR_EQUAL);

	transparentPipeline.pipeline = pipelineBuilder.build_pipeline(engine->_device);

	vkDestroyShaderModule(engine->_device, meshFragShader, nullptr);
	vkDestroyShaderModule(engine->_device, meshVertexShader, nullptr);
	engine->_mainDeletionQueue.push_function([=]() {
		// 1. 销毁具体的管线实体
		vkDestroyPipeline(engine->_device, opaquePipeline.pipeline, nullptr);
		vkDestroyPipeline(engine->_device, transparentPipeline.pipeline, nullptr);

		// 2. 销毁管线的总布线图 (Pipeline Layout)
		// newLayout 在上面被 [=] 按值捕获了，所以在这里绝对安全
		vkDestroyPipelineLayout(engine->_device, newLayout, nullptr);
	});

}

uint32_t VulkanEngine::upload_bindless_texture(VkImageView view, VkSampler sampler)
{
    uint32_t id = bindlessTextureCount++;
    DescriptorWriter writer;
    writer.write_image_to_array(1, id, view, sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.update_set(_device, _bindlessDescriptorSet);
    return id;
}

uint32_t VulkanEngine::upload_bindless_material(const GLTFMetallic_Roughness::MaterialConstants& materialData)
{
    uint32_t id = bindlessMaterialCount++;
    // Buffer 创建时已设置 VMA_ALLOCATION_CREATE_MAPPED_BIT，直接使用持久映射指针
    GLTFMetallic_Roughness::MaterialConstants* matArray =
        (GLTFMetallic_Roughness::MaterialConstants*)_materialBuffer.allocation->GetMappedData();
    matArray[id] = materialData;
    return id;
}

void GLTFMetallic_Roughness::clear_resources(VkDevice device)
{
}

MaterialInstance GLTFMetallic_Roughness::write_material(VkDevice device, MaterialPass pass, const MaterialResources& resources, DescriptorAllocatorGrowable& descriptorAllocator)
{
    MaterialInstance matData;
    matData.passType = pass;
    if (pass == MaterialPass::Transparent) {
        matData.pipeline = &transparentPipeline;
    }
    else {
        matData.pipeline = &opaquePipeline;
    }

    VulkanEngine& engine = VulkanEngine::Get();

    // 1. Upload textures to bindless array and get IDs
    uint32_t colorID = engine.upload_bindless_texture(resources.colorImage.imageView, resources.colorSampler);
    uint32_t metalRoughID = engine.upload_bindless_texture(resources.metalRoughImage.imageView, resources.metalRoughSampler);
    uint32_t normalID = engine.upload_bindless_texture(resources.normalImage.imageView, resources.normalSampler);
    uint32_t occlusionID = engine.upload_bindless_texture(resources.occlusionImage.imageView, resources.occlusionSampler);
    uint32_t emissiveID = engine.upload_bindless_texture(resources.emissiveImage.imageView, resources.emissiveSampler);

    // Read the material data that was populated by the asset pipeline
    MaterialConstants localMat = resources.data;

    // Update the struct with global texture IDs
    localMat.colorTexID = colorID;
    localMat.metalRoughTexID = metalRoughID;
    localMat.normalTexID = normalID;
    localMat.occlusionTexID = occlusionID;
    localMat.emissiveTexID = emissiveID;

    // 2. Upload material to global SSBO and get materialID
    matData.materialID = engine.upload_bindless_material(localMat);

    return matData;
}
void VulkanEngine::init_default_data() {


    // auto result = loadScene(this, "../assets_export/basicmesh.pfb");

    // if (result.has_value()) {
    //     for (auto& [k, v] : result.value()->meshes) {
    //         testMeshes.push_back(v);
    //     }
    // } else {
    //     fmt::print("Warning: Could not load basicmesh.pfb\n");
    // }

    //3 default textures, white, grey, black. 1 pixel each
    uint32_t white = glm::packUnorm4x8(glm::vec4(1, 1, 1, 1));
    _whiteImage = create_image((void*)&white, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT);

    uint32_t grey = glm::packUnorm4x8(glm::vec4(0.66f, 0.66f, 0.66f, 1));
    _greyImage = create_image((void*)&grey, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT);

    uint32_t black = glm::packUnorm4x8(glm::vec4(0, 0, 0, 0));
    _blackImage = create_image((void*)&black, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT);

    // Default normal map (0.5, 0.5, 1.0)
    uint32_t defaultNormal = glm::packUnorm4x8(glm::vec4(0.5f, 0.5f, 1.0f, 1.0f));
    _defaultNormalImage = create_image((void*)&defaultNormal, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT);

    //checkerboard image
    uint32_t magenta = glm::packUnorm4x8(glm::vec4(1, 0, 1, 1));
    std::array<uint32_t, 16 *16 > pixels; //for 16x16 checkerboard texture
    for (int x = 0; x < 16; x++) {
        for (int y = 0; y < 16; y++) {
            pixels[y*16 + x] = ((x % 2) ^ (y % 2)) ? magenta : black;
        }
    }
    _errorCheckerboardImage = create_image(pixels.data(), VkExtent3D{16, 16, 1}, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT);

    VkSamplerCreateInfo sampl = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};

    sampl.magFilter = VK_FILTER_NEAREST;
    sampl.minFilter = VK_FILTER_NEAREST;

    vkCreateSampler(_device, &sampl, nullptr, &_defaultSamplerNearest);

    sampl.magFilter = VK_FILTER_LINEAR;
    sampl.minFilter = VK_FILTER_LINEAR;
    vkCreateSampler(_device, &sampl, nullptr, &_defaultSamplerLinear);

    _mainDeletionQueue.push_function([&](){
        vkDestroySampler(_device,_defaultSamplerNearest,nullptr);
        vkDestroySampler(_device,_defaultSamplerLinear,nullptr);

        destroy_image(_whiteImage);
        destroy_image(_greyImage);
        destroy_image(_blackImage);
        destroy_image(_errorCheckerboardImage);
        destroy_image(_defaultNormalImage);
    });

	// for (auto& m : testMeshes) {
	// 	std::shared_ptr<MeshNode> newNode = std::make_shared<MeshNode>();
	// 	newNode->mesh = m;
	//
	// 	newNode->localTransform = glm::mat4{ 1.f };
	// 	newNode->worldTransform = glm::mat4{ 1.f };
	//
	// 	for (auto& s : newNode->mesh->surfaces) {
	// 		s.material = std::make_shared<Material>(defaultData);
	// 	}
	//
	// 	loadedNodes[m->name] = std::move(newNode);
	// }

}
void VulkanEngine::create_swapchain(uint32_t width, uint32_t height)
{
    vkb::SwapchainBuilder swapchainBuilder{ _chosenGPU,_device,_surface };

    _swapchainImageFormat = VK_FORMAT_B8G8R8A8_UNORM;

    vkb::Swapchain vkbSwapchain = swapchainBuilder
        //.use_default_format_selection()
        .set_desired_format(VkSurfaceFormatKHR{ .format = _swapchainImageFormat, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
        //use vsync present mode
        // .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
		.set_desired_present_mode(VK_PRESENT_MODE_MAILBOX_KHR)
        .set_desired_extent(width, height)
        .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        .build()
        .value();

    _swapchainExtent = vkbSwapchain.extent;
    //store swapchain and its related images
    _swapchain = vkbSwapchain.swapchain;
    _swapchainImages = vkbSwapchain.get_images().value();
    _swapchainImageViews = vkbSwapchain.get_image_views().value();
}

void VulkanEngine::init_swapchain()
{
    create_swapchain(_windowExtent.width, _windowExtent.height);
    //draw image size will match the window
    VkExtent3D drawImageExtent = {
        _windowExtent.width,
        _windowExtent.height,
        1
    };

    //hardcoding the draw format to 32 bit float
    _drawImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    _drawImage.imageExtent = drawImageExtent;

    VkImageUsageFlags drawImageUsages{};
    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_STORAGE_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    VkImageCreateInfo rimg_info = vkinit::image_create_info(_drawImage.imageFormat, drawImageUsages, drawImageExtent);

    //for the draw image, we want to allocate it from gpu local memory
    VmaAllocationCreateInfo rimg_allocinfo = {};
    rimg_allocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    rimg_allocinfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    //allocate and create the image
    vmaCreateImage(_allocator, &rimg_info, &rimg_allocinfo, &_drawImage.image, &_drawImage.allocation, nullptr);

    //build a image-view for the draw image to use for rendering
    VkImageViewCreateInfo rview_info = vkinit::imageview_create_info(_drawImage.imageFormat, _drawImage.image, VK_IMAGE_ASPECT_COLOR_BIT);

    VK_CHECK(vkCreateImageView(_device, &rview_info, nullptr, &_drawImage.imageView));



    _depthImage.imageFormat = VK_FORMAT_D32_SFLOAT;
    _depthImage.imageExtent = drawImageExtent;
    VkImageUsageFlags depthImageUsages{};
    depthImageUsages |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depthImageUsages |= VK_IMAGE_USAGE_SAMPLED_BIT;

    VkImageCreateInfo dimg_info = vkinit::image_create_info(_depthImage.imageFormat, depthImageUsages, drawImageExtent);

    //allocate and create the image
    vmaCreateImage(_allocator, &dimg_info, &rimg_allocinfo, &_depthImage.image, &_depthImage.allocation, nullptr);

    //build a image-view for the draw image to use for rendering
    VkImageViewCreateInfo dview_info = vkinit::imageview_create_info(_depthImage.imageFormat, _depthImage.image, VK_IMAGE_ASPECT_DEPTH_BIT);

    VK_CHECK(vkCreateImageView(_device, &dview_info, nullptr, &_depthImage.imageView));


    //add to deletion queues
    _mainDeletionQueue.push_function([=]() {
        vkDestroyImageView(_device, _drawImage.imageView, nullptr);
        vmaDestroyImage(_allocator, _drawImage.image, _drawImage.allocation);

        vkDestroyImageView(_device, _depthImage.imageView, nullptr);
        vmaDestroyImage(_allocator, _depthImage.image, _depthImage.allocation);
    });


}
void VulkanEngine::init_gbuffer() {
    VkExtent3D gbufferExtent = { _windowExtent.width, _windowExtent.height, 1 };
	VmaAllocationCreateInfo vmaallocInfo = {};
	vmaallocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	vmaallocInfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // 1. Albedo (RGBA8)
    VkImageCreateInfo albedoInfo = vkinit::image_create_info(
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        gbufferExtent);

    // _gAlbedo = create_image(albedoInfo, VMA_MEMORY_USAGE_GPU_ONLY);
    vmaCreateImage(_allocator, &albedoInfo, &vmaallocInfo, &_gAlbedo.image, &_gAlbedo.allocation, nullptr);

    VkImageViewCreateInfo albedoViewInfo = vkinit::imageview_create_info(
        VK_FORMAT_R8G8B8A8_UNORM, _gAlbedo.image, VK_IMAGE_ASPECT_COLOR_BIT);
    vkCreateImageView(_device, &albedoViewInfo, nullptr, &_gAlbedo.imageView);

    // 2. Normal (RGBA16F - 高精度法线极其重要，避免光照条纹)
    VkImageCreateInfo normalInfo = vkinit::image_create_info(
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        gbufferExtent);
    vmaCreateImage(_allocator, &normalInfo, &vmaallocInfo, &_gNormal.image, &_gNormal.allocation, nullptr);

    VkImageViewCreateInfo normalViewInfo = vkinit::imageview_create_info(
        VK_FORMAT_R16G16B16A16_SFLOAT, _gNormal.image, VK_IMAGE_ASPECT_COLOR_BIT);
    vkCreateImageView(_device, &normalViewInfo, nullptr, &_gNormal.imageView);

    // 3. ORM (AO, Roughness, Metallic) (RGBA8)
    VkImageCreateInfo ormInfo = vkinit::image_create_info(
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        gbufferExtent);
    vmaCreateImage(_allocator, &ormInfo, &vmaallocInfo, &_gORM.image, &_gORM.allocation, nullptr);

    VkImageViewCreateInfo ormViewInfo = vkinit::imageview_create_info(
        VK_FORMAT_R8G8B8A8_UNORM, _gORM.image, VK_IMAGE_ASPECT_COLOR_BIT);
    vkCreateImageView(_device, &ormViewInfo, nullptr, &_gORM.imageView);

    // 把它们加进删除队列
    _mainDeletionQueue.push_function([=, this]() {
        vkDestroyImageView(_device, _gAlbedo.imageView, nullptr);
        vmaDestroyImage(_allocator, _gAlbedo.image, _gAlbedo.allocation);

        vkDestroyImageView(_device, _gNormal.imageView, nullptr);
        vmaDestroyImage(_allocator, _gNormal.image, _gNormal.allocation);

        vkDestroyImageView(_device, _gORM.imageView, nullptr);
        vmaDestroyImage(_allocator, _gORM.image, _gORM.allocation);
    });

    // --- Create G-Buffer Descriptor Layout (For deferred lighting pass) ---
    DescriptorLayoutBuilder builder;
    builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // gAlbedo
    builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // gNormal
    builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // gMaterial
    builder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // gDepth

    _gBufferDescriptorLayout = builder.build(_device, VK_SHADER_STAGE_FRAGMENT_BIT);
    
    _mainDeletionQueue.push_function([=, this]() {
        vkDestroyDescriptorSetLayout(_device, _gBufferDescriptorLayout, nullptr);
    });

    // --- Allocate & Write G-Buffer Descriptor Set ---
    _gBufferDescriptorSet = globalDescriptorAllocator.allocate(_device, _gBufferDescriptorLayout);
    
    DescriptorWriter gBufferWriter;
    // Note: uses _defaultSamplerNearest which is now correctly initialized because init_gbuffer() is called after init_default_data()
    gBufferWriter.write_image(0, _gAlbedo.imageView, _defaultSamplerNearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    gBufferWriter.write_image(1, _gNormal.imageView, _defaultSamplerNearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    gBufferWriter.write_image(2, _gORM.imageView, _defaultSamplerNearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    gBufferWriter.write_image(3, _depthImage.imageView, _defaultSamplerNearest, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    
    gBufferWriter.update_set(_device, _gBufferDescriptorSet);
}
void VulkanEngine::destroy_swapchain()
{
    vkDestroySwapchainKHR(_device, _swapchain, nullptr);

    // destroy swapchain resources
    for (int i = 0; i < _swapchainImageViews.size(); i++) {

        vkDestroyImageView(_device, _swapchainImageViews[i], nullptr);
    }
}

void VulkanEngine::resize_swapchain()
{
    vkDeviceWaitIdle(_device);

    destroy_swapchain();

    int w, h;
    SDL_GetWindowSize(_window, &w, &h);
    _windowExtent.width = w;
    _windowExtent.height = h;

    create_swapchain(_windowExtent.width, _windowExtent.height);

    resize_requested = false;
}

void VulkanEngine::update_scene()
{


	glm::mat4 view = mainCamera.getViewMatrix();

	// camera projection
	glm::mat4 projection = glm::perspective(glm::radians(70.f), (float)_windowExtent.width / (float)_windowExtent.height, 10000.f, 0.1f);

	// invert the Y direction on projection matrix so that we are more similar
	// to opengl and gltf axis
	projection[1][1] *= -1;
	sceneData.view = view;
	sceneData.proj = projection;
	sceneData.viewproj = projection * view;

	// loadedNodes["Suzanne"]->Draw(glm::mat4{1.f}, mainDrawContext);
	// for (int x = -3; x < 3; x++) {
	//
	// 	glm::mat4 scale = glm::scale(glm::vec3{0.2});
	// 	glm::mat4 translation =  glm::translate(glm::vec3{x, 1, 0});
	//
	// 	loadedNodes["Cube"]->Draw(translation * scale, mainDrawContext);
	// }
	 if (loadedScenes.find("structure") != loadedScenes.end()) {
	     loadedScenes["structure"]->Draw(glm::mat4{ 1.f }, mainDrawContext);
	 }


	//some default lighting parameters
	sceneData.ambientColor = glm::vec4(.1f);
	sceneData.sunlightColor = glm::vec4(1.f);
	sceneData.sunlightDirection = glm::vec4(0,1,0.5,1.f);
}
