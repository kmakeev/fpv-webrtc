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

# ── OpenXR loader (prebuilt, pulled from Quest 2 via ADB) ───────────────────
#
# Meta SDK Maven (sdk.developer.oculus.com) may be inaccessible from CI / dev
# machines without a Meta developer account or corporate proxy.
# Solution: pull the loader binary directly from the Quest 2 system partition —
# it is the same binary that Meta's Maven AAR would provide:
#
#   adb pull /system/lib64/libopenxr_loader.so \
#       app/src/main/jniLibs/arm64-v8a/libopenxr_loader.so
#
# The file is in .gitignore (binary, device-specific) — each developer pulls it
# once from their own Quest 2.  See setup-openxr.sh for the full setup script.
#
# OpenXR HEADERS come from the Khronos Prefab AAR (Maven Central, accessible):
#   org.khronos.openxr:openxr_loader_for_android:1.1.49
# The AAR exposes headers via the "headers" Prefab module (no .so needed from it).

find_package(OpenXR REQUIRED CONFIG)   # headers only (from Khronos Prefab AAR)

# Prebuilt Meta loader .so from jniLibs/ — packaged into the APK by AGP automatically.
# CMake also needs it at link time so the linker can resolve OpenXR symbols.
add_library(openxr_loader_prebuilt SHARED IMPORTED)
set_target_properties(openxr_loader_prebuilt PROPERTIES
    IMPORTED_LOCATION
    "${CMAKE_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libopenxr_loader.so"
)

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
    OpenXR::headers          # Khronos headers-only Prefab target
    openxr_loader_prebuilt   # Meta loader .so from jniLibs/
)
