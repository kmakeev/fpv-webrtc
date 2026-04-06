/**
 * video_state.h — Cross-module accessors for the current video OES texture.
 *
 * Allows xr_renderer.cpp to read the video texture state from video_decoder.cpp
 * without going through JNI (both compile into the same libfpv-native.so).
 *
 * TASK-004 stores the latest OES texture ID and SurfaceTexture transform matrix
 * in module-static globals; these functions expose them with proper mutex protection.
 */

#pragma once
#include <GLES3/gl3.h>

/**
 * Return the current OES texture ID (0 if no frame has arrived yet).
 * Thread-safe (atomic read).
 */
GLuint videostate_getTexId();

/**
 * Copy the current SurfaceTexture 3×3 transform matrix into out[9].
 * Format: row-major float[9] from android.graphics.Matrix.getValues().
 * Thread-safe (mutex-protected copy).
 */
void videostate_getMatrix(float out[9]);
