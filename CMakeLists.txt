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

# 2. 强制指定搜索目录并打印出来调试
set(SHADER_SEARCH_PATH "${PROJECT_SOURCE_DIR}/shaders")
message(STATUS "Searching shaders in: ${SHADER_SEARCH_PATH}")

file(GLOB_RECURSE GLSL_SOURCE_FILES CONFIGURE_DEPENDS
        "${SHADER_SEARCH_PATH}/*.frag"
        "${SHADER_SEARCH_PATH}/*.vert"
        "${SHADER_SEARCH_PATH}/*.comp"
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

# 5. 关键：确保目标始终被构建
add_custom_target(
        Shaders ALL
        DEPENDS ${SPIRV_BINARY_FILES}
)

# 确保 engine 链接前先跑编译
if(TARGET engine)
    add_dependencies(engine Shaders)
endif()
