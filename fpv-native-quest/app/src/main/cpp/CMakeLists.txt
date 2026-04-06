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

# Native shared library (loaded by MainActivity via System.loadLibrary("fpv-native"))
add_library(fpv-native SHARED
    xr_renderer.cpp    # TODO TASK-005: OpenXR session + stereo rendering
    video_decoder.cpp  # TASK-004: SurfaceTexture OES texture + transform matrix storage
)

target_include_directories(fpv-native PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(fpv-native
    ${log-lib}
    ${egl-lib}
    ${gles-lib}
    ${android-lib}
    ${mediandk-lib}
)

# Future: link OpenXR loader when Meta XR SDK is integrated (TASK-005)
# find_package(OpenXR REQUIRED)
# target_link_libraries(fpv-native openxr_loader)
