package com.fpv.quest

import android.app.Activity
import android.util.Log
import org.webrtc.EglBase

/**
 * Dedicated thread for the OpenXR frame loop (TASK-005).
 *
 * Responsibilities:
 *   1. Creates an EGL context that shares textures with the WebRTC decoder context
 *      (eglBase), so the OES texture stored in g_videoTexId (TASK-004) is accessible.
 *   2. Makes the shared context current before calling nativeInitXR() — the native
 *      code uses eglGetCurrentContext() to bind the OpenXR session to this context.
 *   3. Runs the XR frame loop: nativeRenderFrame() blocks inside xrWaitFrame()
 *      for natural pacing (~13.9 ms at 72 Hz); returns false when the session ends.
 *   4. Calls nativeDestroyXR() after the loop exits and releases the EGL context.
 *
 * Lifecycle:
 *   Start in Activity.onResume(); stop (via stopRendering()) in Activity.onPause().
 *   The thread runs until nativeRenderFrame() returns false (session exiting)
 *   or stopRendering() calls nativeRequestExitSession() to force an early exit.
 */
class XrRenderThread(
    private val activity: Activity,
    /** WebRTC decoder's EGL context — we share textures with it. */
    private val webRtcEglContext: EglBase.Context
) : Thread("XrRenderThread") {

    companion object {
        private const val TAG = "XrRenderThread"
    }

    @Volatile private var stopRequested = false
    private var sharedEgl: EglBase? = null

    override fun run() {
        Log.i(TAG, "started")

        // ── 1. Create shared EGL context ──────────────────────────────────────
        // Sharing with webRtcEglContext puts both contexts in the same EGL share
        // group → OES textures allocated by WebRTC's SurfaceTextureHelper are
        // accessible here (Adreno/Quest 2 supports cross-context OES sampling
        // within the same share group).
        val egl = try {
            EglBase.create(webRtcEglContext).also { sharedEgl = it }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to create shared EGL context: $e")
            return
        }

        // 1×1 dummy pbuffer — needed to make the context current without a window.
        // OpenXR will render to its own swapchain framebuffers, not this surface.
        try {
            egl.createDummyPbufferSurface()
            egl.makeCurrent()
        } catch (e: Exception) {
            Log.e(TAG, "Failed to make EGL context current: $e")
            egl.release()
            sharedEgl = null
            return
        }

        // ── 2. Initialize OpenXR session ──────────────────────────────────────
        val initOk = MainActivity.nativeInitXR(activity)
        if (!initOk) {
            Log.e(TAG, "nativeInitXR failed — OpenXR unavailable")
            egl.detachCurrent()
            egl.release()
            sharedEgl = null
            return
        }
        Log.i(TAG, "OpenXR initialized")

        // ── 3. Frame loop ─────────────────────────────────────────────────────
        // nativeRenderFrame() calls xrWaitFrame() which provides natural pacing.
        // Returns false when the XR session is exiting.
        try {
            while (!stopRequested) {
                if (!MainActivity.nativeRenderFrame()) {
                    Log.i(TAG, "nativeRenderFrame returned false — session exiting")
                    break
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Exception in frame loop: $e")
        }

        // ── 4. Cleanup ────────────────────────────────────────────────────────
        Log.i(TAG, "frame loop exited — destroying XR")
        MainActivity.nativeDestroyXR()

        try {
            egl.detachCurrent()
        } catch (_: Exception) {}
        egl.release()
        sharedEgl = null

        Log.i(TAG, "stopped")
    }

    /**
     * Request the render loop to stop.
     * Calls nativeRequestExitSession() to unblock a pending xrWaitFrame(),
     * then sets the stop flag so the loop exits cleanly on the next iteration.
     */
    fun stopRendering() {
        stopRequested = true
        // Ask the XR runtime to deliver XR_SESSION_STATE_STOPPING promptly,
        // which makes the next nativeRenderFrame() return false.
        try {
            MainActivity.nativeRequestExitSession()
        } catch (_: Exception) {}
    }
}
