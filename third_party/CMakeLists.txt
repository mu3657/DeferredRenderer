find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)
include(ExternalProject)

add_library(vkbootstrap STATIC)
add_library(glm INTERFACE)
add_library(vma INTERFACE)
add_library(renderdoc INTERFACE)

add_library(stb_image INTERFACE)

add_subdirectory(fastgltf)
add_subdirectory(fmt EXCLUDE_FROM_ALL)
add_subdirectory(SDL EXCLUDE_FROM_ALL)

# RTXGI-DDGI remains an isolated NVIDIA-licensed dependency. The renderer owns
# the Vulkan resources and descriptors; the SDK supplies the canonical host
# data packing, indexing, and DDGI update shaders.
set(RTXGI_STATIC_LIB ON CACHE BOOL "" FORCE)
set(RTXGI_API_D3D12_ENABLE OFF CACHE BOOL "" FORCE)
set(RTXGI_API_VULKAN_ENABLE ON CACHE BOOL "" FORCE)
set(RTXGI_COORDINATE_SYSTEM "Right Hand, Y-Up" CACHE STRING "" FORCE)
set(RTXGI_DDGI_RESOURCE_MANAGEMENT OFF CACHE BOOL "" FORCE)
set(RTXGI_DDGI_USE_SHADER_CONFIG_FILE OFF CACHE BOOL "" FORCE)
add_subdirectory(rtxgi-ddgi/rtxgi-sdk EXCLUDE_FROM_ALL)

target_sources(vkbootstrap PRIVATE 
  vkbootstrap/VkBootstrap.h
  vkbootstrap/VkBootstrap.cpp
  )

target_include_directories(vkbootstrap PUBLIC vkbootstrap)
target_link_libraries(vkbootstrap PUBLIC Vulkan::Vulkan $<$<BOOL:UNIX>:${CMAKE_DL_LIBS}>)
set_property(TARGET vkbootstrap PROPERTY CXX_STANDARD 20)
target_include_directories(renderdoc INTERFACE Renderdoc)
#both vma and glm and header only libs so we only need the include path
target_include_directories(vma INTERFACE vma)

target_include_directories(glm INTERFACE glm)


#add_library(sdl2 INTERFACE)

#target_include_directories(sdl2 INTERFACE $ENV{VULKAN_SDK}/Include/SDL2 )
#target_link_directories(sdl2 INTERFACE $ENV{VULKAN_SDK}/Lib )

#target_link_libraries(sdl2 INTERFACE SDL2 SDL2main)

add_library(imgui STATIC)

target_include_directories(imgui PUBLIC imgui)

target_sources(imgui PRIVATE 
  imgui/imgui.h
  imgui/imgui.cpp

  imgui/imgui_demo.cpp
  imgui/imgui_draw.cpp
  imgui/imgui_widgets.cpp
  imgui/imgui_tables.cpp
  imgui/imgui_impl_vulkan.cpp
  imgui/imgui_impl_sdl2.cpp
  imgui/ImGuizmo.h
  imgui/ImGuizmo.cpp
  )

target_link_libraries(imgui PUBLIC Vulkan::Vulkan SDL2::SDL2)

target_include_directories(stb_image INTERFACE stb_image)
add_library(lz4 STATIC
        lz4/lib/lz4.c
        lz4/lib/lz4hc.c
        lz4/lib/lz4frame.c
)
target_include_directories(lz4 PUBLIC lz4/lib)


# Additional third party libraries for asset-baker
add_library(json INTERFACE)
target_include_directories(json INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/nlohmann")

add_library(tinyobjloader STATIC)
target_sources(tinyobjloader PRIVATE tinyobjloader/tiny_obj_loader.cc)
target_include_directories(tinyobjloader PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/tinyobjloader")
set(NV_LIB_DIR "${PROJECT_SOURCE_DIR}/libs")
ExternalProject_Add(nvtools
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/nvidia-texture-tools"
    BINARY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/nvlibs"
    CMAKE_ARGS -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY=${PROJECT_SOURCE_DIR}/libs -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=${PROJECT_SOURCE_DIR}/libs -DCMAKE_CXX_STANDARD=17
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DNVTT_USE_CUDA=OFF
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS
        "${NV_LIB_DIR}/nvtt.lib"
        "${NV_LIB_DIR}/nvcore.lib"
        "${NV_LIB_DIR}/nvimage.lib"
        "${NV_LIB_DIR}/nvmath.lib"
        "${NV_LIB_DIR}/nvsquish.lib"
        "${NV_LIB_DIR}/nvthread.lib"
        "${NV_LIB_DIR}/posh.lib"
        "${NV_LIB_DIR}/bc6h.lib"
        "${NV_LIB_DIR}/bc7.lib"
)

add_library(nvtt INTERFACE)
add_dependencies(nvtt nvtools)
target_include_directories(nvtt INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/nvidia-texture-tools/src/nvtt")

set(nvttfolder "${PROJECT_SOURCE_DIR}/libs/")

target_link_libraries(nvtt INTERFACE "${nvttfolder}/nvtt.lib")
target_link_libraries(nvtt INTERFACE "${nvttfolder}/bc6h.lib")
target_link_libraries(nvtt INTERFACE "${nvttfolder}/bc7.lib")
# target_link_libraries(nvtt INTERFACE "${nvttfolder}/EtcLib.lib")
target_link_libraries(nvtt INTERFACE "${nvttfolder}/nvcore.lib")
target_link_libraries(nvtt INTERFACE "${nvttfolder}/nvimage.lib")
target_link_libraries(nvtt INTERFACE "${nvttfolder}/nvmath.lib")
target_link_libraries(nvtt INTERFACE "${nvttfolder}/nvsquish.lib")
target_link_libraries(nvtt INTERFACE "${nvttfolder}/nvthread.lib")
target_link_libraries(nvtt INTERFACE "${nvttfolder}/posh.lib")

add_library(assimp SHARED IMPORTED GLOBAL)
	set_target_properties(assimp PROPERTIES
            IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/assimp/assimp-vc143-mt.dll"

            # IMPLIB 指向 lib (链接时用) - 请修改这里！
            IMPORTED_IMPLIB   "${CMAKE_CURRENT_SOURCE_DIR}/assimp/assimp-vc143-mt.lib"
	)

target_include_directories(assimp INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/assimp/include")

add_library(tinyGLTF INTERFACE)
target_include_directories(tinyGLTF INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/tinygltf")
 
 add_library(tracy STATIC tracy/public/TracyClient.cpp)
 target_include_directories(tracy PUBLIC tracy/public)
 target_compile_definitions(tracy PUBLIC TRACY_ENABLE)
 if(WIN32)
     target_link_libraries(tracy PUBLIC ws2_32 dbghelp secur32)
 endif()

add_library(entt INTERFACE)
target_include_directories(entt INTERFACE entt)
