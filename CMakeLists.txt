# CMakeList.txt : CMake project for vulkan_guide, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 4.0)

project ("vulkan_engine")
# --- 1. MSVC 全域設定 (必須放在 add_subdirectory 之前) ---
if(MSVC)
    # 強制所有目標使用動態 Runtime 庫 (Debug 下為 /MDd)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

    # 解決編碼警告 C4819 (防止亂碼與編譯錯誤)
    add_compile_options("/utf-8")
endif()

find_package(Vulkan REQUIRED)
# 強制 SDL2 不要排除標準庫
set(SDL_LIBC ON CACHE BOOL "" FORCE)
# 確保 MSVC 運行庫一致
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
add_subdirectory(third_party)

set (CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin")
set (CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin")

add_subdirectory(src)
add_subdirectory(assetlib)
add_subdirectory(asset-baker)
# 1. 明确找到编译器
find_program(GLSL_VALIDATOR glslangValidator HINTS $ENV{VULKAN_SDK}/Bin)
find_program(DXC_EXECUTABLE dxc HINTS $ENV{VULKAN_SDK}/Bin)

# 2. 强制指定搜索目录并打印出来调试
set(SHADER_SEARCH_PATH "${PROJECT_SOURCE_DIR}/shaders")
message(STATUS "Searching shaders in: ${SHADER_SEARCH_PATH}")

file(GLOB_RECURSE GLSL_SOURCE_FILES CONFIGURE_DEPENDS
        "${SHADER_SEARCH_PATH}/*.frag"
        "${SHADER_SEARCH_PATH}/*.vert"
        "${SHADER_SEARCH_PATH}/*.comp"
)

file(GLOB_RECURSE HLSL_SOURCE_FILES CONFIGURE_DEPENDS
        "${SHADER_SEARCH_PATH}/*.comp.hlsl"
)

# 3. 检查是否找到了文件
list(LENGTH GLSL_SOURCE_FILES SHADER_COUNT)
message(STATUS "Found ${SHADER_COUNT} shader files.")

set(SPIRV_BINARY_FILES "")

# 4. 创建输出目录
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/shaders")

foreach(GLSL ${GLSL_SOURCE_FILES})
    get_filename_component(FILE_NAME ${GLSL} NAME)
    set(SPIRV "${CMAKE_BINARY_DIR}/shaders/${FILE_NAME}.spv")

    message(STATUS "Configuring command for: ${FILE_NAME}")

    add_custom_command(
            OUTPUT ${SPIRV}
            COMMAND ${GLSL_VALIDATOR} -V "${GLSL}" -o "${SPIRV}"
            DEPENDS ${GLSL}
            COMMENT "Compiling ${FILE_NAME}"
            VERBATIM
    )
    list(APPEND SPIRV_BINARY_FILES ${SPIRV})
endforeach()

foreach(HLSL ${HLSL_SOURCE_FILES})
    if(NOT DXC_EXECUTABLE)
        message(FATAL_ERROR "DXC is required to compile ${HLSL}")
    endif()

    get_filename_component(FILE_NAME ${HLSL} NAME)
    set(SPIRV "${CMAKE_BINARY_DIR}/shaders/${FILE_NAME}.spv")

    message(STATUS "Configuring DXC command for: ${FILE_NAME}")

    add_custom_command(
            OUTPUT ${SPIRV}
            COMMAND ${DXC_EXECUTABLE}
                    -spirv
                    -T cs_6_0
                    -E main
                    -fspv-target-env=vulkan1.3
                    -Fo "${SPIRV}"
                    "${HLSL}"
            DEPENDS ${HLSL}
                    "${PROJECT_SOURCE_DIR}/src/bendsss/bend_sss_gpu.h"
            COMMENT "Compiling ${FILE_NAME} with DXC"
            VERBATIM
    )
    list(APPEND SPIRV_BINARY_FILES ${SPIRV})
endforeach()

# Compile the canonical RTXGI probe blending kernels as Vulkan SPIR-V. These
# binaries are intentionally kept separate from the active custom DDGI path
# until the runtime resources use RTXGI's Texture2DArray layout and packed
# constants ABI.
set(RTXGI_DDGI_SHADER_DIR
    "${PROJECT_SOURCE_DIR}/third_party/rtxgi-ddgi/rtxgi-sdk/shaders")
file(GLOB_RECURSE RTXGI_DDGI_SHADER_DEPENDENCIES CONFIGURE_DEPENDS
    "${RTXGI_DDGI_SHADER_DIR}/*.hlsl"
    "${PROJECT_SOURCE_DIR}/third_party/rtxgi-ddgi/rtxgi-sdk/include/rtxgi/*.h")

function(add_rtxgi_ddgi_blend_shader OUTPUT_NAME BLEND_RADIANCE OUTPUT_BINDING PROBE_TEXELS PROBE_INTERIOR_TEXELS)
    if(NOT DXC_EXECUTABLE)
        message(FATAL_ERROR "DXC is required to compile the RTXGI-DDGI shaders")
    endif()

    set(SPIRV "${CMAKE_BINARY_DIR}/shaders/${OUTPUT_NAME}.spv")
    add_custom_command(
        OUTPUT ${SPIRV}
        COMMAND ${DXC_EXECUTABLE}
            -spirv
            -T cs_6_0
            -E DDGIProbeBlendingCS
            -fspv-target-env=vulkan1.3
            -I "${RTXGI_DDGI_SHADER_DIR}"
            -DHLSL=1
            -DRTXGI_COORDINATE_SYSTEM=2
            -DRTXGI_DDGI_RESOURCE_MANAGEMENT=0
            -DRTXGI_DDGI_SHADER_REFLECTION=0
            -DRTXGI_DDGI_BINDLESS_RESOURCES=0
            -DVOLUME_CONSTS_REGISTER=0
            -DVOLUME_CONSTS_SPACE=0
            -DRAY_DATA_REGISTER=1
            -DRAY_DATA_SPACE=0
            -DOUTPUT_REGISTER=${OUTPUT_BINDING}
            -DOUTPUT_SPACE=0
            -DPROBE_DATA_REGISTER=3
            -DPROBE_DATA_SPACE=0
            -DPROBE_VARIABILITY_REGISTER=4
            -DPROBE_VARIABILITY_SPACE=0
            -DRTXGI_PUSH_CONSTS_TYPE=1
            -DRTXGI_DECLARE_PUSH_CONSTS=1
            -DRTXGI_DDGI_BLEND_SHARED_MEMORY=1
            -DRTXGI_DDGI_BLEND_RAYS_PER_PROBE=128
            -DRTXGI_DDGI_BLEND_SCROLL_SHARED_MEMORY=0
            -DRTXGI_DDGI_BLEND_USE_ROOT_DISPATCH_BASE=1
            -DRTXGI_DDGI_BLEND_RADIANCE=${BLEND_RADIANCE}
            -DRTXGI_DDGI_PROBE_NUM_TEXELS=${PROBE_TEXELS}
            -DRTXGI_DDGI_PROBE_NUM_INTERIOR_TEXELS=${PROBE_INTERIOR_TEXELS}
            -Fo "${SPIRV}"
            "${RTXGI_DDGI_SHADER_DIR}/ddgi/ProbeBlendingCS.hlsl"
        DEPENDS ${RTXGI_DDGI_SHADER_DEPENDENCIES}
        COMMENT "Compiling official RTXGI-DDGI ${OUTPUT_NAME}"
        VERBATIM)
    set(SPIRV_BINARY_FILES ${SPIRV_BINARY_FILES} ${SPIRV} PARENT_SCOPE)
endfunction()

add_rtxgi_ddgi_blend_shader(
    rtxgi_ddgi_irradiance_blend.comp.hlsl 1 2 10 8)
add_rtxgi_ddgi_blend_shader(
    rtxgi_ddgi_distance_blend.comp.hlsl 0 5 18 16)

function(add_rtxgi_ddgi_probe_management_shader OUTPUT_NAME SOURCE_NAME ENTRY_POINT)
    set(SPIRV "${CMAKE_BINARY_DIR}/shaders/${OUTPUT_NAME}.spv")
    add_custom_command(
        OUTPUT ${SPIRV}
        COMMAND ${DXC_EXECUTABLE}
            -spirv
            -T cs_6_0
            -E ${ENTRY_POINT}
            -fspv-target-env=vulkan1.3
            -I "${RTXGI_DDGI_SHADER_DIR}"
            -DHLSL=1
            -DRTXGI_COORDINATE_SYSTEM=2
            -DRTXGI_DDGI_RESOURCE_MANAGEMENT=0
            -DRTXGI_DDGI_SHADER_REFLECTION=0
            -DRTXGI_DDGI_BINDLESS_RESOURCES=0
            -DVOLUME_CONSTS_REGISTER=0
            -DVOLUME_CONSTS_SPACE=0
            -DRAY_DATA_REGISTER=1
            -DRAY_DATA_SPACE=0
            -DPROBE_DATA_REGISTER=3
            -DPROBE_DATA_SPACE=0
            -DRTXGI_PUSH_CONSTS_TYPE=1
            -DRTXGI_DECLARE_PUSH_CONSTS=1
            -Fo "${SPIRV}"
            "${RTXGI_DDGI_SHADER_DIR}/ddgi/${SOURCE_NAME}"
        DEPENDS ${RTXGI_DDGI_SHADER_DEPENDENCIES}
        COMMENT "Compiling official RTXGI-DDGI ${OUTPUT_NAME}"
        VERBATIM)
    set(SPIRV_BINARY_FILES ${SPIRV_BINARY_FILES} ${SPIRV} PARENT_SCOPE)
endfunction()

add_rtxgi_ddgi_probe_management_shader(
    rtxgi_ddgi_probe_relocation.comp.hlsl
    ProbeRelocationCS.hlsl
    DDGIProbeRelocationCS)
add_rtxgi_ddgi_probe_management_shader(
    rtxgi_ddgi_probe_relocation_reset.comp.hlsl
    ProbeRelocationCS.hlsl
    DDGIProbeRelocationResetCS)
add_rtxgi_ddgi_probe_management_shader(
    rtxgi_ddgi_probe_classification.comp.hlsl
    ProbeClassificationCS.hlsl
    DDGIProbeClassificationCS)
add_rtxgi_ddgi_probe_management_shader(
    rtxgi_ddgi_probe_classification_reset.comp.hlsl
    ProbeClassificationCS.hlsl
    DDGIProbeClassificationResetCS)

# 5. 关键：确保目标始终被构建
add_custom_target(
        Shaders ALL
        DEPENDS ${SPIRV_BINARY_FILES}
)

# 确保 engine 链接前先跑编译
if(TARGET engine)
    add_dependencies(engine Shaders)
endif()
