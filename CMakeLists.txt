cmake_minimum_required(VERSION 3.24)
project(SnowMinimap LANGUAGES C CXX ASM_MASM)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "SnowRunner is x64 only; configure with an x64 toolchain")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the DLL must not depend on whatever runtime the player has installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(SNOWMINIMAP_DEPLOY_DIR "" CACHE PATH "SnowRunner Sources\\Bin folder to copy version.dll into after each build")

include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9b
    GIT_SHALLOW TRUE)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR do-not-configure) # sources are compiled below, its own CMake install rules are not needed
FetchContent_Declare(miniz
    URL https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
    SOURCE_SUBDIR do-not-configure)
FetchContent_MakeAvailable(imgui minhook miniz)

add_library(third_party STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c
    ${miniz_SOURCE_DIR}/miniz.c)
target_include_directories(third_party PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${minhook_SOURCE_DIR}/include
    ${miniz_SOURCE_DIR})
target_compile_definitions(third_party PUBLIC
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS
    WIN32_LEAN_AND_MEAN NOMINMAX)

# Shaders are compiled ahead of time so the mod does not need d3dcompiler at runtime for them.
find_program(FXC fxc REQUIRED HINTS
    "$ENV{WindowsSdkVerBinPath}/x64"
    "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64")
set(SHADER_SRC ${CMAKE_SOURCE_DIR}/src/render/minimap.hlsl)
set(SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
add_custom_command(
    OUTPUT ${SHADER_OUT}/minimap_vs.h ${SHADER_OUT}/minimap_ps.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT}
    COMMAND ${FXC} /nologo /O3 /T vs_5_0 /E VSMain /Vn g_minimapVS /Fh ${SHADER_OUT}/minimap_vs.h ${SHADER_SRC}
    COMMAND ${FXC} /nologo /O3 /T ps_5_0 /E PSMain /Vn g_minimapPS /Fh ${SHADER_OUT}/minimap_ps.h ${SHADER_SRC}
    DEPENDS ${SHADER_SRC}
    VERBATIM)

add_library(SnowMinimap SHARED
    src/dllmain.cpp
    src/proxy/version_proxy.cpp
    src/proxy/version_stubs.asm
    src/proxy/version.def
    src/core/log.cpp
    src/core/config.cpp
    src/core/paths.cpp
    src/dev/renderdoc.cpp
    src/game/memory.cpp
    src/game/game_state.cpp
    src/hooks/present_hook.cpp
    src/render/minimap_renderer.cpp
    src/render/game_terrain.cpp
    src/render/map_texture.cpp
    src/render/pak_assets.cpp
    src/ui/overlay.cpp
    ${SHADER_OUT}/minimap_vs.h
    ${SHADER_OUT}/minimap_ps.h)
set_target_properties(SnowMinimap PROPERTIES OUTPUT_NAME version)
target_include_directories(SnowMinimap PRIVATE src ${SHADER_OUT} third_party/renderdoc)
target_compile_options(SnowMinimap PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8>")
target_link_libraries(SnowMinimap PRIVATE third_party d3d11 dxgi d3dcompiler dwmapi windowscodecs delayimp)
# Nothing graphics-related may load while the loader is still resolving version.dll for the game.
target_link_options(SnowMinimap PRIVATE
    /DELAYLOAD:d3d11.dll /DELAYLOAD:dxgi.dll /DELAYLOAD:d3dcompiler_47.dll /DELAYLOAD:dwmapi.dll)

if(SNOWMINIMAP_DEPLOY_DIR)
    file(TO_CMAKE_PATH "${SNOWMINIMAP_DEPLOY_DIR}" deploy_dir)
    add_custom_command(TARGET SnowMinimap POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DSRC_DIR=$<TARGET_FILE_DIR:SnowMinimap> -DDEPLOY_DIR=${deploy_dir}
                -P ${CMAKE_SOURCE_DIR}/cmake/deploy.cmake
        VERBATIM)
endif()
