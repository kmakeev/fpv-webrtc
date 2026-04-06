cmake_minimum_required(VERSION 3.22.1)
project(fpv-native)

# C++17 for structured bindings, std::optional, etc.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Android system libraries
find_library(log-lib      log)       # __android_log_print
find_library(egl-lib      EGL)       # EGL context for OpenGL ES
find_library(gles-lib     GLESv3)    # OpenGL ES 3.2
find_library(android-lib  android)   # ANativeWindow, AAssetManager
find_library(mediandk-lib mediandk)  # AMediaCodec, AMediaFormat (API 29+)

# OpenXR loader — provided by Khronos Maven Central Prefab AAR
# (org.khronos.openxr:openxr_loader_for_android in app/build.gradle).
# Prefab package name: "OpenXR"; CMake target: OpenXR::openxr_loader
# Headers at: prefab/modules/headers/include/openxr/
find_package(OpenXR REQUIRED CONFIG)

# Native shared library (loaded by MainActivity via System.loadLibrary("fpv-native"))
add_library(fpv-native SHARED
    xr_renderer.cpp    # TASK-005: OpenXR session + stereo rendering
    video_decoder.cpp  # TASK-004: SurfaceTexture OES texture + transform matrix storage
)

target_include_directories(fpv-native PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(fpv-native PRIVATE
    XR_USE_PLATFORM_ANDROID
    XR_USE_GRAPHICS_API_OPENGL_ES
)

target_link_libraries(fpv-native
    ${log-lib}
    ${egl-lib}
    ${gles-lib}
    ${android-lib}
    ${mediandk-lib}
    OpenXR::openxr_loader   # Khronos OpenXR loader (from Prefab AAR, Maven Central)
)
