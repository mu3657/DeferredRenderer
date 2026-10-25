cmake_minimum_required(VERSION 3.24)

set(SCRIPTING_STANDALONE OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(renderer_scripting LANGUAGES C CXX)
    set(SCRIPTING_STANDALONE ON)
endif()

include(FetchContent)

# Pinned upstream archives. FetchContent also supports local source overrides:
# FETCHCONTENT_SOURCE_DIR_SCRIPTING_LUA_SOURCE / ...SCRIPTING_SOL2_SOURCE.
FetchContent_Declare(scripting_lua_source
    URL https://www.lua.org/ftp/lua-5.4.9.tar.gz
    URL_HASH SHA256=2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(scripting_sol2_source
    URL https://codeload.github.com/ThePhD/sol2/tar.gz/refs/tags/v3.3.1
    URL_HASH SHA256=ad121461047d52b449aa84234a6b578fa3ed95d67d1a0703902eba72417f61bb
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    # Use only the headers, without sol2's own dependency/build configuration.
    SOURCE_SUBDIR unused
)
FetchContent_MakeAvailable(scripting_lua_source scripting_sol2_source)

file(GLOB SCRIPTING_LUA_SOURCES CONFIGURE_DEPENDS "${scripting_lua_source_SOURCE_DIR}/src/*.c")
list(REMOVE_ITEM SCRIPTING_LUA_SOURCES
    "${scripting_lua_source_SOURCE_DIR}/src/lua.c"
    "${scripting_lua_source_SOURCE_DIR}/src/luac.c"
)
add_library(scripting_lua STATIC ${SCRIPTING_LUA_SOURCES})
target_include_directories(scripting_lua SYSTEM PUBLIC "${scripting_lua_source_SOURCE_DIR}/src")
target_compile_definitions(scripting_lua PRIVATE _CRT_SECURE_NO_WARNINGS)
if(UNIX)
    target_compile_definitions(scripting_lua PRIVATE LUA_USE_POSIX LUA_USE_DLOPEN)
    target_link_libraries(scripting_lua PRIVATE ${CMAKE_DL_LIBS} m)
endif()

if(NOT TARGET glm)
    add_library(glm INTERFACE)
    target_include_directories(glm SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/glm")
endif()

add_library(scripting STATIC script_system.cpp script_bindings.cpp script_config.cpp
    script_system.h script_host.h script_types.h script_config.h)
target_compile_features(scripting PUBLIC cxx_std_20)
target_include_directories(scripting PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(scripting SYSTEM PRIVATE "${scripting_sol2_source_SOURCE_DIR}/include")
target_include_directories(scripting SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/nlohmann")
target_link_libraries(scripting PUBLIC glm PRIVATE scripting_lua)
target_compile_definitions(scripting PRIVATE SOL_ALL_SAFETIES_ON=1)
if(MSVC)
    target_compile_options(scripting PRIVATE /W4 /utf-8)
endif()

option(SCRIPTING_BUILD_TESTS "Build the standalone scripting validation" ${SCRIPTING_STANDALONE})
option(SCRIPTING_BUILD_EXAMPLE "Build the standalone scripting example" ${SCRIPTING_STANDALONE})
if(SCRIPTING_BUILD_TESTS)
    enable_testing()
    add_executable(scripting_validation tests/script_system_tests.cpp)
    target_link_libraries(scripting_validation PRIVATE scripting)
    add_test(NAME scripting_validation COMMAND scripting_validation)
endif()
if(SCRIPTING_BUILD_EXAMPLE)
    add_executable(scripting_demo examples/main.cpp)
    target_link_libraries(scripting_demo PRIVATE scripting)
    target_compile_definitions(scripting_demo PRIVATE
        SCRIPTING_EXAMPLE_FILE="${CMAKE_CURRENT_SOURCE_DIR}/examples/timer.lua")
endif()
