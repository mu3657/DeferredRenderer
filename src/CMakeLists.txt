
# Add source to this project's executable.
add_executable (engine 
  main.cpp
  scene_script_host.cpp
  scene_scripts.cpp
  script_inspector.cpp
  scripting/tests/scene_scripting_validation.cpp
  ../tests/taa_validation.cpp
  ../tests/taa_sponza_validation.cpp
  ../tests/clustered_validation.cpp
  ../tests/cornell_validation.cpp
  light_system.h
  light_system.cpp
  vk_types.h
  vk_initializers.cpp
  vk_initializers.h
  vk_images.h
  vk_images.cpp 
  vk_descriptors.h
  vk_descriptors.cpp
  vk_descriptor_system.h
  vk_descriptor_system.cpp
  vk_pipelines.h
  vk_pipelines.cpp
  pipeline_registry.h
  pipeline_registry.cpp
  material_system.h
  material_system.cpp
  vk_engine.h
  editor_layout.h
  vk_engine.cpp
  environment_lighting.h
  environment_lighting.cpp
  vk_loader.h
  vk_loader.cpp
  camera.cpp
  camera.h
        GpuData.h
        vk_types.cpp
        asset_manager.h
        asset_manager.cpp
        vk_scene.h
        vk_scene.cpp
        vk_outliner.h
        vk_outliner.cpp
        Renderpasses/shadow_pass.h
        render_pass.h
        Renderpasses/geometry_pass.h
        Renderpasses/geometry_pass.cpp
        Renderpasses/light_binning_pass.h
        Renderpasses/light_binning_pass.cpp
        Renderpasses/shadow_pass.cpp
        Renderpasses/transparent_pass.h
        Renderpasses/transparent_pass.cpp
        Renderpasses/ContactShadow_pass.h
        Renderpasses/ContactShadow_pass.cpp
        Renderpasses/ddgi_probe_trace_pass.h
        Renderpasses/ddgi_probe_trace_pass.cpp
        Renderpasses/ddgi_probe_blend_pass.h
        Renderpasses/ddgi_probe_blend_pass.cpp
        Renderpasses/ddgi_probe_debug_pass.h
        Renderpasses/ddgi_probe_debug_pass.cpp
        Renderpasses/tone_map_pass.h
        Renderpasses/tone_map_pass.cpp
        Renderpasses/taa_pass.h
        Renderpasses/taa_pass.cpp
        GlobalilluminationStructure/ddgi_volume.h
        GlobalilluminationStructure/ddgi_volume.cpp
        GlobalilluminationStructure/ray_tracing_scene.h
        GlobalilluminationStructure/ray_tracing_scene.cpp
)

set_property(TARGET engine PROPERTY CXX_STANDARD 20)
target_compile_definitions(engine PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE)
target_include_directories(engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(engine PUBLIC vma glm Vulkan::Vulkan fmt::fmt stb_image SDL2::SDL2 vkbootstrap imgui fastgltf::fastgltf lz4 assetlib json assimp renderdoc tracy entt RTXGI-VK)
target_link_libraries(engine PRIVATE scripting)
target_compile_definitions(engine PRIVATE SCRIPTING_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/src/scripting/examples")
if(MSVC)
  # Optimize only the validation harness's CPU readback/statistics loops.
  # Its Debug CRT/ABI match engine; renderer runtime checks remain enabled.
  add_library(ddgi_validation_cpu OBJECT ../tests/ddgi_validation.cpp)
  set_target_properties(ddgi_validation_cpu PROPERTIES CXX_STANDARD 20 MSVC_RUNTIME_CHECKS "")
  target_compile_options(ddgi_validation_cpu PRIVATE /O2)
  target_compile_definitions(ddgi_validation_cpu PRIVATE $<TARGET_PROPERTY:engine,COMPILE_DEFINITIONS>)
  target_include_directories(ddgi_validation_cpu PRIVATE $<TARGET_PROPERTY:engine,INCLUDE_DIRECTORIES>)
  get_target_property(ddgi_validation_libraries engine LINK_LIBRARIES)
  target_link_libraries(ddgi_validation_cpu PRIVATE ${ddgi_validation_libraries})
  target_sources(engine PRIVATE $<TARGET_OBJECTS:ddgi_validation_cpu>)
else()
  target_sources(engine PRIVATE ../tests/ddgi_validation.cpp)
endif()
 target_compile_definitions(engine PUBLIC TRACY_ENABLE)


target_precompile_headers(engine PUBLIC <optional> <vector> <memory> <string> <vector> <unordered_map> <glm/mat4x4.hpp>  <glm/vec4.hpp> <vulkan/vulkan.h>)

if(WIN32)
  add_custom_command(TARGET engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:engine> $<TARGET_FILE_DIR:engine>
    COMMAND_EXPAND_LISTS
    )
endif()
