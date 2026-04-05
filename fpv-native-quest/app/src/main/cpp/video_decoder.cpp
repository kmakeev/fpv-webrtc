/**
 * video_decoder.cpp — Zero-copy video decode via MediaCodec + SurfaceTexture stub.
 *
 * TASK-002: compiles cleanly, no functionality.
 * TODO TASK-004: implement the zero-copy H.264 decode path.
 *
 * Target architecture (replaces async browser MediaCodec pipeline):
 *
 *   libwebrtc (io.github.webrtc-sdk:android)
 *       └── HardwareVideoDecoderFactory
 *               └── MediaCodec (H.264, synchronous-mode, KEY_LATENCY=0)
 *                       └── Surface output
 *                               └── SurfaceTexture (GL_TEXTURE_EXTERNAL_OES)
 *                                       └── EGLImageKHR  ← zero-copy GPU path
 *                                               └── OpenGL ES 3.2 (xr_renderer)
 *
 * Why zero-copy matters:
 *   Browser path:  MediaCodec → CPU buffer → glTexImage2D (CPU→GPU copy, ~2–5ms)
 *   Native path:   MediaCodec → Surface → SurfaceTexture.updateTexImage() (no CPU copy)
 *   Saving: ~2–5 ms per frame, eliminates synchronization stalls.
 *
 * Key Android APIs:
 *   - AMediaCodec_createDecoderByType("video/avc")     (NDK media API)
 *   - AMediaCodec_setOutputSurface(codec, surface)     (direct Surface output)
 *   - SurfaceTexture.setDefaultBufferSize(width, height)
 *   - SurfaceTexture.updateTexImage()                  (bind latest frame to OES texture)
 *   - glBindTexture(GL_TEXTURE_EXTERNAL_OES, texId)    (sample in fragment shader)
 *
 * Fragment shader snippet for OES texture sampling:
 *   #extension GL_OES_EGL_image_external_essl3 : require
 *   uniform samplerExternalOES uVideoTex;
 *   vec4 color = texture(uVideoTex, vTexCoord);
 */

#include <jni.h>
#include <android/log.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>  // GL_TEXTURE_EXTERNAL_OES

// TODO TASK-004: #include <media/NdkMediaCodec.h>
// TODO TASK-004: #include <media/NdkMediaFormat.h>

#define LOG_TAG "video_decoder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" {

/**
 * Create an OES external texture for SurfaceTexture output.
 * Returns the GL texture ID to be passed back to Java for SurfaceTexture creation.
 *
 * TODO TASK-004:
 *   GLuint texId;
 *   glGenTextures(1, &texId);
 *   glBindTexture(GL_TEXTURE_EXTERNAL_OES, texId);
 *   glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
 *   glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
 *   glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
 *   glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
 *   return (jint) texId;
 */
JNIEXPORT jint JNICALL
Java_com_fpv_quest_MainActivity_nativeCreateVideoTexture(JNIEnv* env, jobject thiz) {
    LOGI("nativeCreateVideoTexture() stub — TODO TASK-004");
    return 0;
}

/**
 * Called from Java after SurfaceTexture.updateTexImage().
 * Retrieves the surface transform matrix and passes it to the renderer.
 *
 * TODO TASK-004:
 *   float stMatrix[16];
 *   // received from SurfaceTexture.getTransformMatrix(stMatrix)
 *   // pass to xr_renderer to correct texture coordinates
 */
JNIEXPORT void JNICALL
Java_com_fpv_quest_MainActivity_nativeUpdateVideoFrame(JNIEnv* env, jobject thiz,
                                                        jfloatArray transformMatrix) {
    // TODO TASK-004
}

} // extern "C"
