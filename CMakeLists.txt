cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 NEW)
project(gtaRenderHook LANGUAGES CXX)

if (CMAKE_SIZEOF_VOID_P AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "gtaRenderHook targets 32-bit GTA San Andreas (v1.0 US, Win32/x86). Reconfigure with -A Win32 (or a 32-bit x86 toolchain).")
endif ()

file(TO_CMAKE_PATH "$ENV{PLUGIN_SDK_DIR}" _default_plugin_sdk_dir)
if (NOT _default_plugin_sdk_dir AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../build_cache/deps/plugin-sdk")
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../build_cache/deps/plugin-sdk" _default_plugin_sdk_dir)
endif ()
if (NOT "$ENV{ANT_TWEAKBAR}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{ANT_TWEAKBAR}" _default_anttweakbar_dir)
elseif (NOT "$ENV{ANT_TWEAKBAR_DIR}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{ANT_TWEAKBAR_DIR}" _default_anttweakbar_dir)
elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../build_cache/deps/AntTweakBar")
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../build_cache/deps/AntTweakBar" _default_anttweakbar_dir)
endif ()
if (NOT "$ENV{TINY_XML}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{TINY_XML}" _default_tinyxml2_dir)
elseif (NOT "$ENV{TINYXML2_DIR}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{TINYXML2_DIR}" _default_tinyxml2_dir)
elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../build_cache/deps/tinyxml2")
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../build_cache/deps/tinyxml2" _default_tinyxml2_dir)
endif ()
if (NOT "$ENV{FASTNOISESIMD_DIR}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{FASTNOISESIMD_DIR}" _default_fastnoisesimd_dir)
elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../build_cache/deps/FastNoiseSIMD/FastNoiseSIMD")
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../build_cache/deps/FastNoiseSIMD/FastNoiseSIMD" _default_fastnoisesimd_dir)
endif ()

set(GTA_SA_FOLDER "" CACHE PATH "GTA SA installation or staging folder")
set(PLUGIN_SDK_DIR "${_default_plugin_sdk_dir}" CACHE PATH "Path to plugin-sdk root")
set(ANT_TWEAKBAR_DIR "${_default_anttweakbar_dir}" CACHE PATH "Path to AntTweakBar root")
set(TINYXML2_DIR "${_default_tinyxml2_dir}" CACHE PATH "Path to tinyxml2 root")
set(FASTNOISESIMD_DIR "${_default_fastnoisesimd_dir}" CACHE PATH "Path to FastNoiseSIMD root")

if (NOT PLUGIN_SDK_DIR AND _default_plugin_sdk_dir)
    set(PLUGIN_SDK_DIR "${_default_plugin_sdk_dir}" CACHE PATH "Path to plugin-sdk root" FORCE)
endif ()
if (NOT ANT_TWEAKBAR_DIR AND _default_anttweakbar_dir)
    set(ANT_TWEAKBAR_DIR "${_default_anttweakbar_dir}" CACHE PATH "Path to AntTweakBar root" FORCE)
endif ()
if (NOT TINYXML2_DIR AND _default_tinyxml2_dir)
    set(TINYXML2_DIR "${_default_tinyxml2_dir}" CACHE PATH "Path to tinyxml2 root" FORCE)
endif ()
if (NOT FASTNOISESIMD_DIR AND _default_fastnoisesimd_dir)
    set(FASTNOISESIMD_DIR "${_default_fastnoisesimd_dir}" CACHE PATH "Path to FastNoiseSIMD root" FORCE)
endif ()

foreach (_path_var IN ITEMS GTA_SA_FOLDER PLUGIN_SDK_DIR ANT_TWEAKBAR_DIR TINYXML2_DIR FASTNOISESIMD_DIR)
    if (${_path_var})
        file(TO_CMAKE_PATH "${${_path_var}}" ${_path_var})
    endif ()
endforeach ()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

file(GLOB GTARENDERHOOK_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/gtaRenderHook/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/gtaRenderHook/*.h"
)
list(FILTER GTARENDERHOOK_SOURCES EXCLUDE REGEX "(/|\\\\)(D3DEngine|FastNoiseSIMD.*)\\.(cpp|h)$")

add_library(gtaRenderHook SHARED ${GTARENDERHOOK_SOURCES})

set_target_properties(gtaRenderHook PROPERTIES
    OUTPUT_NAME "gtaRenderHook"
    SUFFIX ".asi"
)

target_include_directories(gtaRenderHook PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/gtaRenderHook"
)

if (ANT_TWEAKBAR_DIR)
    target_include_directories(gtaRenderHook PRIVATE "${ANT_TWEAKBAR_DIR}/include")
    target_link_directories(gtaRenderHook PRIVATE "${ANT_TWEAKBAR_DIR}/lib")
endif ()

if (PLUGIN_SDK_DIR)
    target_include_directories(gtaRenderHook PRIVATE
        "${PLUGIN_SDK_DIR}/plugin_sa"
        "${PLUGIN_SDK_DIR}/plugin_sa/game_sa"
        "${PLUGIN_SDK_DIR}/plugin_sa/game_sa/enums"
        "${PLUGIN_SDK_DIR}/plugin_sa/game_sa/rw"
        "${PLUGIN_SDK_DIR}/safetyhook"
        "${PLUGIN_SDK_DIR}/shared"
        "${PLUGIN_SDK_DIR}/shared/game"
    )
    target_link_directories(gtaRenderHook PRIVATE "${PLUGIN_SDK_DIR}/output/lib")
endif ()

if (TINYXML2_DIR)
    target_include_directories(gtaRenderHook PRIVATE
        "${TINYXML2_DIR}"
        "${TINYXML2_DIR}/.."
    )
    target_link_directories(gtaRenderHook PRIVATE
        "$<$<CONFIG:Debug>:${TINYXML2_DIR}/Debug-Lib>"
        "$<$<NOT:$<CONFIG:Debug>>:${TINYXML2_DIR}/Release-Lib>"
        "${TINYXML2_DIR}"
    )
endif ()

if (FASTNOISESIMD_DIR)
    target_include_directories(gtaRenderHook PRIVATE "${FASTNOISESIMD_DIR}")
    target_link_directories(gtaRenderHook PRIVATE
        "$<$<CONFIG:Debug>:${FASTNOISESIMD_DIR}/Debug-Lib>"
        "$<$<NOT:$<CONFIG:Debug>>:${FASTNOISESIMD_DIR}/Release-Lib>"
        "${FASTNOISESIMD_DIR}/lib"
        "${FASTNOISESIMD_DIR}"
    )
endif ()

target_compile_definitions(gtaRenderHook PRIVATE
    WIN32
    _WINDOWS
    _USRDLL
    _MBCS
    GTARENDERHOOK_EXPORTS
    GTA_SA
    GTASA
    PLUGIN_SGV_10US
    _CRT_NON_CONFORMING_SWPRINTFS
    _CRT_SECURE_NO_WARNINGS
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
)

if (MSVC)
    target_compile_options(gtaRenderHook PRIVATE /FI"stdafx.h")
endif ()

target_link_libraries(gtaRenderHook PRIVATE
    d3d11
    dxgi
    d3dcompiler
    dxguid
    user32
    AntTweakBar
    tinyxml2
    FastNoiseSIMD
    $<$<CONFIG:Debug>:plugin_d>
    $<$<NOT:$<CONFIG:Debug>>:plugin>
)

if (GTA_SA_FOLDER)
    install(TARGETS gtaRenderHook
        RUNTIME DESTINATION "${GTA_SA_FOLDER}"
        LIBRARY DESTINATION "${GTA_SA_FOLDER}"
    )
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/gtaRenderHook/shaders"
        DESTINATION "${GTA_SA_FOLDER}"
        PATTERN "cache" EXCLUDE
        PATTERN "*.cso" EXCLUDE
    )
endif ()
