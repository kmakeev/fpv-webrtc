/**
 * xr_renderer.cpp — OpenXR stereo renderer stub.
 *
 * TASK-002: compiles cleanly, no functionality.
 * TODO TASK-004: implement stereo VR rendering to replace webxr-renderer.js.
 *
 * Target architecture:
 *   - OpenXR 1.0 session lifecycle (xrCreateInstance, xrCreateSession, etc.)
 *   - OpenGL ES 3.2 swapchain (XR_KHR_opengl_es_enable)
 *   - Stereo views via xrLocateViews() for left/right eye projection matrices
 *   - Full-screen quad rendering: SurfaceTexture (OES external) → both eye viewports
 *   - Stats HUD overlay: texture quad in world space (0.5m wide, 0.8m forward)
 *   - IPD: read from XrViewConfigurationView (device-accurate, not hardcoded 0.064m)
 *
 * Key improvement over webxr-renderer.js:
 *   - No browser compositor layer overhead (~5–10 ms saved)
 *   - Direct OpenXR ATW (Asynchronous TimeWarp) at display refresh rate
 *   - SurfaceTexture zero-copy path: video frame stays in GPU memory (see video_decoder.cpp)
 */

#include <jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define LOG_TAG "xr_renderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// TODO TASK-004: #include <openxr/openxr.h>
// TODO TASK-004: #include <openxr/openxr_platform.h>

extern "C" {

/**
 * Initialize OpenXR instance and session.
 * Called from JNI once EGL context is available.
 *
 * TODO TASK-004:
 *   XrInstance instance;
 *   XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
 *   // fill applicationInfo, enabledExtensionNames (XR_KHR_opengl_es_enable, etc.)
 *   xrCreateInstance(&createInfo, &instance);
 *
 *   XrSystemId systemId;
 *   XrSystemGetInfo sysInfo = {XR_TYPE_SYSTEM_GET_INFO};
 *   sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
 *   xrGetSystem(instance, &sysInfo, &systemId);
 *
 *   // Create session with EGL context binding
 *   XrGraphicsBindingOpenGLESAndroidKHR binding = {XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
 *   binding.display = eglGetCurrentDisplay();
 *   binding.config  = eglGetCurrentSurface(EGL_DRAW);
 *   binding.context = eglGetCurrentContext();
 *   XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
 *   sessionInfo.next = &binding;
 *   sessionInfo.systemId = systemId;
 *   xrCreateSession(instance, &sessionInfo, &session);
 */
JNIEXPORT void JNICALL
Java_com_fpv_quest_MainActivity_nativeInitXR(JNIEnv* env, jobject thiz) {
    LOGI("nativeInitXR() stub — TODO TASK-004");
}

/**
 * Render one stereo frame.
 * Called per-frame from the main loop after video frame is available in SurfaceTexture.
 *
 * TODO TASK-004:
 *   xrWaitFrame(session, &waitFrameInfo, &frameState);
 *   xrBeginFrame(session, nullptr);
 *
 *   XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
 *   locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
 *   locateInfo.displayTime = frameState.predictedDisplayTime;
 *   locateInfo.space = appSpace;
 *   xrLocateViews(session, &locateInfo, &viewState, 2, &viewCount, views);
 *
 *   // For each eye: bind swapchain image, set viewport, draw video quad + HUD
 *   for (int eye = 0; eye < 2; ++eye) {
 *       glViewport(eyeViewport[eye]);
 *       drawVideoQuad(videoTexture, views[eye].fov, views[eye].pose);
 *       drawStatsHUD(statsTexture, views[eye]);
 *   }
 *
 *   xrEndFrame(session, &endFrameInfo);
 */
JNIEXPORT void JNICALL
Java_com_fpv_quest_MainActivity_nativeRenderFrame(JNIEnv* env, jobject thiz) {
    // TODO TASK-004
}

JNIEXPORT void JNICALL
Java_com_fpv_quest_MainActivity_nativeDestroyXR(JNIEnv* env, jobject thiz) {
    LOGI("nativeDestroyXR() stub — TODO TASK-004");
    // TODO TASK-004: xrDestroySession(session); xrDestroyInstance(instance);
}

} // extern "C"
