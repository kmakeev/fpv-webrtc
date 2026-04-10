/**
 * video_decoder.cpp — Zero-copy video decode via SurfaceTexture + OES texture.
 *
 * TASK-004: Intercepts VideoFrame.TextureBuffer (OES) delivered by
 * HardwareVideoDecoderFactory to store the current texture ID and
 * SurfaceTexture transform matrix for the OpenXR renderer (TASK-005).
 *
 * Pipeline (zero-copy GPU path):
 *
 *   libwebrtc (HardwareVideoDecoderFactory + shared EGL context)
 *       └── MediaCodec (H.264, surface output mode)
 *               └── SurfaceTexture  ← WebRTC manages this internally
 *                       └── VideoFrame.TextureBuffer (GL_TEXTURE_EXTERNAL_OES)
 *                               └── EglVideoSink.kt calls nativeUpdateVideoFrame()
 *                                       └── g_videoTexId / g_stMatrix stored here
 *                                               └── xr_renderer uses them (TASK-005)
 *
 * No CPU copy anywhere in this path.
 */

#include <jni.h>
#include <android/log.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>  // GL_TEXTURE_EXTERNAL_OES
#include <atomic>
#include <mutex>
#include <cstring>         // memcpy
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include "video_state.h"

#define LOG_TAG "video_decoder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ── Module state ─────────────────────────────────────────────────────────────

// Current OES texture ID (updated each frame by EglVideoSink via JNI).
// Atomic so xr_renderer can read it from any thread without a lock.
static std::atomic<GLuint> g_videoTexId{0};

// SurfaceTexture transform matrix from android.graphics.Matrix.getValues().
// Row-major 3×3 affine, 9 floats.  Protected by g_matMutex.
// Identity until first real frame arrives.
static float     g_stMatrix[9] = {
    1.f, 0.f, 0.f,   // row 0
    0.f, 1.f, 0.f,   // row 1
    0.f, 0.f, 1.f,   // row 2
};
static std::mutex g_matMutex;

static std::atomic<uint64_t> g_frameCount{0};

// ── JNI functions ─────────────────────────────────────────────────────────────

extern "C" {

/**
 * Create a placeholder GL_TEXTURE_EXTERNAL_OES texture.
 *
 * Called once from MainActivity after the EGL context is ready.
 * The returned ID is stored in g_videoTexId as a fallback until
 * the first real WebRTC frame arrives via nativeUpdateVideoFrame().
 *
 * The EGL context must be current on the calling thread.
 */
JNIEXPORT jint JNICALL
Java_com_fpv_quest_MainActivity_nativeCreateVideoTexture(JNIEnv* env, jobject /*thiz*/) {
    GLuint texId = 0;
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texId);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    g_videoTexId.store(texId);
    LOGI("nativeCreateVideoTexture: placeholder OES tex=%u", texId);
    return static_cast<jint>(texId);
}

/**
 * Update the stored texture ID and transform matrix from a WebRTC OES frame.
 *
 * Called by EglVideoSink.kt on every incoming VideoFrame.TextureBuffer.
 * The textureId is the GL_TEXTURE_EXTERNAL_OES name managed by WebRTC's
 * internal SurfaceTextureHelper.  The transformMatrix is float[9] from
 * android.graphics.Matrix.getValues() (row-major 3×3 affine transform).
 *
 * Thread-safe: may be called from the WebRTC decoder thread.
 */
JNIEXPORT void JNICALL
Java_com_fpv_quest_MainActivity_nativeUpdateVideoFrame(JNIEnv* env, jobject /*thiz*/,
                                                        jint textureId,
                                                        jfloatArray transformMatrix) {
    g_videoTexId.store(static_cast<GLuint>(textureId));

    {
        std::lock_guard<std::mutex> lock(g_matMutex);
        jfloat* mat = env->GetFloatArrayElements(transformMatrix, nullptr);
        if (mat) {
            memcpy(g_stMatrix, mat, 9 * sizeof(float));
            env->ReleaseFloatArrayElements(transformMatrix, mat, JNI_ABORT);
        }
    }

    // Flush GPU commands so the OES texture is visible to other EGL contexts
    // in the same share group (xr_renderer's render thread on Adreno/Quest 2).
    glFlush();

    uint64_t n = ++g_frameCount;
    if (n == 1 || (n % 300) == 0) {
        LOGI("zero-copy frame #%llu: OES tex=%u", (unsigned long long)n, static_cast<GLuint>(textureId));
    }
}

/**
 * Return the current OES texture ID.
 *
 * Called by xr_renderer (TASK-005) each frame to get the texture to sample.
 * Returns 0 if no frame has arrived yet (placeholder may not have OES data).
 */
JNIEXPORT jint JNICALL
Java_com_fpv_quest_MainActivity_nativeGetVideoTextureId(JNIEnv* /*env*/, jobject /*thiz*/) {
    return static_cast<jint>(g_videoTexId.load());
}

/**
 * Copy the current transform matrix (float[9], row-major 3×3 from
 * android.graphics.Matrix) into the provided array.
 * Called by xr_renderer to correct texture coordinates for each frame.
 */
JNIEXPORT void JNICALL
Java_com_fpv_quest_MainActivity_nativeGetVideoTransformMatrix(JNIEnv* env, jobject /*thiz*/,
                                                               jfloatArray outMatrix) {
    std::lock_guard<std::mutex> lock(g_matMutex);
    jfloat* dst = env->GetFloatArrayElements(outMatrix, nullptr);
    if (dst) {
        memcpy(dst, g_stMatrix, 9 * sizeof(float));
        env->ReleaseFloatArrayElements(outMatrix, dst, 0);
    }
}

/**
 * Reset the stored video texture ID to 0.
 * Called from MainActivity.teardown() so that the XR renderer knows there is
 * no active video stream and can show the status overlay instead of a black quad.
 * Does NOT delete the GL texture — the EGL context/WebRTC owns its lifetime.
 */
JNIEXPORT void JNICALL
Java_com_fpv_quest_MainActivity_nativeResetVideoState(JNIEnv* /*env*/, jobject /*thiz*/) {
    g_videoTexId.store(0);
    g_frameCount.store(0);
    LOGI("nativeResetVideoState: g_videoTexId cleared");
}

} // extern "C"

// ── Non-JNI accessors for xr_renderer.cpp ─────────────────────────────────────

GLuint videostate_getTexId() {
    return g_videoTexId.load();
}

void videostate_getMatrix(float out[9]) {
    std::lock_guard<std::mutex> lock(g_matMutex);
    memcpy(out, g_stMatrix, 9 * sizeof(float));
}
