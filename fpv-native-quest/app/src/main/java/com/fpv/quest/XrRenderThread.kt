package com.fpv.quest

import android.app.Activity
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import android.opengl.EGL14
import android.opengl.GLES30
import android.opengl.GLUtils
import android.util.Log
import org.webrtc.EglBase
import java.util.concurrent.atomic.AtomicReference

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
 * Stats overlay (TASK-006):
 *   MainActivity calls updateStats(e2eMs, encMs) or clearStats() from any thread.
 *   The render loop checks pendingStats before each nativeRenderFrame() call and, if a
 *   new value is pending, creates/uploads a Bitmap via GLUtils into a GL_TEXTURE_2D and
 *   notifies C++ via nativeSetStatsTexture().  All GL operations happen on this thread
 *   within the shared EGL context.
 *
 * Lifecycle:
 *   Start in Activity.onResume(); stop (via stopRendering()) in Activity.onDestroy().
 *   The thread runs until nativeRenderFrame() returns false (session exiting)
 *   or stopRendering() calls nativeRequestExitSession() to force an early exit.
 */
class XrRenderThread(
    private val activity: Activity,
    /** WebRTC decoder's EGL context — we share textures with it. */
    private val webRtcEglContext: EglBase.Context,
    /** Server URL to display in the VR connection panel (e.g. "ws://192.168.1.100:8080"). */
    serverUrl: String = "",
    /** Called on the main thread when the user confirms a URL via the VR panel. */
    private val onConnectRequested: ((String) -> Unit)? = null
) : Thread("XrRenderThread") {

    companion object {
        private const val TAG = "XrRenderThread"

        // Sentinel value in pendingStats[0] that means "hide the HUD".
        private const val CLEAR_SENTINEL = Long.MIN_VALUE

        // Stats bitmap dimensions (pixels).
        private const val STATS_W = 256
        private const val STATS_H = 64

        // VR connection panel bitmap dimensions (pixels).
        private const val PANEL_W = 512
        private const val PANEL_H = 160
    }

    // Regex: ws(s)://A.B.C.D:port  or  ws://A.B.C.D  (last octet is editable)
    private val urlRegex = Regex("""^(wss?://\d+\.\d+\.\d+\.)(\d+)(:\d+)?$""")

    init { parseUrl(serverUrl) }

    @Volatile private var stopRequested = false
    private var sharedEgl: EglBase? = null

    // ── Stats overlay state ───────────────────────────────────────────────────
    // Written from any thread; consumed on the render thread before each frame.
    // Element 0: e2eMs (or CLEAR_SENTINEL to hide); element 1: encMs.
    private val pendingStats = AtomicReference<LongArray?>(null)
    private var statsTexId: Int = 0   // GL texture name; 0 = not yet created

    // ── Status overlay state (shown in VR when no video is available) ────────
    // "" (empty) = hide; any text = show that message.
    // AtomicReference null = no pending update.
    private val pendingStatus = AtomicReference<String?>(null)
    private var statusTexId   = 0   // GL texture; 0 = not created

    /**
     * Show a status message in the VR view (visible when there is no video).
     * Pass null to hide the status overlay.  Safe to call from any thread.
     */
    fun showStatus(msg: String?) {
        pendingStatus.set(msg ?: "")   // empty string = hide sentinel
    }

    // ── VR connection panel state (render-thread-only fields) ─────────────────
    private var panelVisible      = false
    private var baseUrl           = ""   // URL prefix  e.g. "ws://192.168.1."
    private var lastOctet         = -1   // editable last octet (0..255), -1 = not parseable
    private var urlSuffix         = ""   // URL suffix  e.g. ":8080"
    private var panelTexId        = 0    // GL texture name; 0 = not yet created
    private var lastOctetChangeMs = 0L   // for thumbstick auto-repeat debounce
    private val inputBuf          = FloatArray(3)  // reused per-frame for nativeGetLastInputState

    /**
     * Request a stats HUD update.  Safe to call from any thread.
     * The upload happens on the render thread on the next frame boundary.
     */
    fun updateStats(e2eMs: Long, encMs: Long) {
        pendingStats.set(longArrayOf(e2eMs, encMs))
    }

    /**
     * Hide the stats HUD.  Safe to call from any thread.
     */
    fun clearStats() {
        pendingStats.set(longArrayOf(CLEAR_SENTINEL, 0L))
    }

    /**
     * Update the server URL shown in the VR connection panel.
     * Safe to call from any thread (the panel reads the fields on the render thread
     * before the next frame, so worst-case one stale frame is shown).
     */
    fun setServerUrl(url: String) {
        parseUrl(url)
    }

    // ── Render thread entry point ─────────────────────────────────────────────

    override fun run() {
        Log.i(TAG, "started")

        // ── 1. Create shared EGL context ──────────────────────────────────────
        // Must use an explicit RGBA8 + ES3 config — EglBase.create(context) without
        // config attributes defaults to CONFIG_PLAIN_PIXEL_BUFFER = {EGL_NONE}, which
        // lets EGL pick any config. On Quest 2 (Adreno 650), that often results in a
        // pixel-buffer-only config that OpenXR rejects with XR_ERROR_GRAPHICS_DEVICE_INVALID.
        //
        // 0x40 = EGL_OPENGL_ES3_BIT (= EGL_OPENGL_ES3_BIT_KHR); not a named constant
        // in android.opengl.EGL14, so use the raw value.
        val xrEglConfig = intArrayOf(
            EGL14.EGL_RED_SIZE,   8,
            EGL14.EGL_GREEN_SIZE, 8,
            EGL14.EGL_BLUE_SIZE,  8,
            EGL14.EGL_ALPHA_SIZE, 8,
            EGL14.EGL_RENDERABLE_TYPE, 0x40,  // EGL_OPENGL_ES3_BIT
            EGL14.EGL_NONE
        )
        val egl = try {
            EglBase.create(webRtcEglContext, xrEglConfig).also { sharedEgl = it }
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
        // Check for pending stats updates before each frame so GL texture uploads
        // happen on this thread (within the shared EGL context).
        try {
            while (!stopRequested) {
                applyPendingStats()
                applyPendingStatus()
                if (!MainActivity.nativeRenderFrame()) {
                    Log.i(TAG, "nativeRenderFrame returned false — session exiting")
                    break
                }
                // Process controller input after nativeRenderFrame (actions already synced in C++)
                nativeGetLastInputState(inputBuf)
                processInput(inputBuf[0] > 0.5f, inputBuf[1] > 0.5f, inputBuf[2])
            }
        } catch (e: Exception) {
            Log.e(TAG, "Exception in frame loop: $e")
        }

        // ── 4. Cleanup ────────────────────────────────────────────────────────
        Log.i(TAG, "frame loop exited — destroying XR")
        MainActivity.nativeDestroyXR()

        // Delete GL textures while the EGL context is still current.
        if (statsTexId != 0) {
            GLES30.glDeleteTextures(1, intArrayOf(statsTexId), 0)
            statsTexId = 0
        }
        if (panelTexId != 0) {
            GLES30.glDeleteTextures(1, intArrayOf(panelTexId), 0)
            panelTexId = 0
        }
        if (statusTexId != 0) {
            GLES30.glDeleteTextures(1, intArrayOf(statusTexId), 0)
            statusTexId = 0
        }

        try {
            egl.detachCurrent()
        } catch (_: Exception) {}
        egl.release()
        sharedEgl = null

        Log.i(TAG, "stopped")
    }

    /**
     * Consume a pending stats update (if any) and apply it:
     * - upload a fresh Bitmap to the stats GL texture, or
     * - hide the HUD by setting the texture ID to 0 in native code.
     *
     * Must be called on the render thread (EGL context must be current).
     */
    private fun applyPendingStats() {
        val stats = pendingStats.getAndSet(null) ?: return
        if (stats[0] == CLEAR_SENTINEL) {
            Log.i(TAG, "clearStats: hiding HUD (texId=0)")
            nativeSetStatsTexture(0)
        } else {
            Log.i(TAG, "applyPendingStats: e2e=${stats[0]}ms enc=${stats[1]}ms → uploadStatsBitmap")
            uploadStatsBitmap(stats[0], stats[1])
        }
    }

    /**
     * Create or update the stats GL_TEXTURE_2D from a freshly drawn Android Bitmap.
     * The Bitmap is drawn with Android Canvas (no native font library needed).
     */
    private fun uploadStatsBitmap(e2eMs: Long, encMs: Long) {
        val bmp = Bitmap.createBitmap(STATS_W, STATS_H, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bmp)

        // Semi-transparent dark background
        val bgPaint = Paint().apply { color = Color.argb(160, 0, 0, 0) }
        canvas.drawRoundRect(
            RectF(0f, 0f, STATS_W.toFloat(), STATS_H.toFloat()),
            8f, 8f, bgPaint
        )

        val textPaint = Paint().apply {
            color = Color.WHITE
            textSize = 22f
            isAntiAlias = true
            typeface = Typeface.MONOSPACE
        }
        canvas.drawText("E2E: ${e2eMs}ms", 10f, 26f, textPaint)
        canvas.drawText("enc: ${encMs}ms", 10f, 54f, textPaint)

        // Create the GL texture on first use
        if (statsTexId == 0) {
            val ids = IntArray(1)
            GLES30.glGenTextures(1, ids, 0)
            statsTexId = ids[0]
            Log.i(TAG, "Created stats GL texture id=$statsTexId")
            GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, statsTexId)
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MIN_FILTER, GLES30.GL_LINEAR)
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MAG_FILTER, GLES30.GL_LINEAR)
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_S, GLES30.GL_CLAMP_TO_EDGE)
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_T, GLES30.GL_CLAMP_TO_EDGE)
            nativeSetStatsTexture(statsTexId)
            Log.i(TAG, "nativeSetStatsTexture($statsTexId) called")
        }

        GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, statsTexId)
        GLUtils.texImage2D(GLES30.GL_TEXTURE_2D, 0, bmp, 0)
        val glErr = GLES30.glGetError()
        if (glErr != GLES30.GL_NO_ERROR) Log.e(TAG, "GL error after texImage2D: 0x${glErr.toString(16)}")
        bmp.recycle()
        // Always re-register with C++ — after clearStats() sets g_statsTexId=0 the
        // texture still exists in GL, so on reconnect we must re-notify native.
        nativeSetStatsTexture(statsTexId)
    }

    // ── Status overlay ────────────────────────────────────────────────────────

    private fun applyPendingStatus() {
        val msg = pendingStatus.getAndSet(null) ?: return
        if (msg.isEmpty()) {
            nativeSetStatusTexture(0)
            if (statusTexId != 0) {
                GLES30.glDeleteTextures(1, intArrayOf(statusTexId), 0)
                statusTexId = 0
            }
        } else {
            uploadStatusBitmap(msg)
        }
    }

    private fun uploadStatusBitmap(msg: String) {
        val w = 512; val h = 80
        val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bmp)
        val bg = Paint().apply { color = Color.argb(200, 0, 0, 0) }
        canvas.drawRoundRect(RectF(0f, 0f, w.toFloat(), h.toFloat()), 12f, 12f, bg)
        val tp = Paint().apply {
            color = Color.WHITE
            textSize = 21f
            isAntiAlias = true
            typeface = Typeface.MONOSPACE
            textAlign = Paint.Align.CENTER
        }
        // Word-wrap at ≈ 42 chars per line (two lines max)
        val words = msg.split(" ")
        val lines = mutableListOf<String>()
        var line = ""
        for (w2 in words) {
            val candidate = if (line.isEmpty()) w2 else "$line $w2"
            if (candidate.length > 42 && line.isNotEmpty()) { lines += line; line = w2 }
            else line = candidate
        }
        if (line.isNotEmpty()) lines += line
        val lineH = 26f
        val totalH = lines.size * lineH
        val startY = (h - totalH) / 2f + lineH
        lines.forEachIndexed { i, l -> canvas.drawText(l, w / 2f, startY + i * lineH, tp) }

        if (statusTexId == 0) {
            val ids = IntArray(1)
            GLES30.glGenTextures(1, ids, 0)
            statusTexId = ids[0]
            GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, statusTexId)
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MIN_FILTER, GLES30.GL_LINEAR)
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MAG_FILTER, GLES30.GL_LINEAR)
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_S, GLES30.GL_CLAMP_TO_EDGE)
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_T, GLES30.GL_CLAMP_TO_EDGE)
        }
        GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, statusTexId)
        GLUtils.texImage2D(GLES30.GL_TEXTURE_2D, 0, bmp, 0)
        bmp.recycle()
        nativeSetStatusTexture(statusTexId)
    }

    // ── URL parsing + VR panel logic ──────────────────────────────────────────

    private fun parseUrl(url: String) {
        val m = urlRegex.matchEntire(url.trim())
        if (m != null) {
            baseUrl   = m.groupValues[1]
            lastOctet = m.groupValues[2].toIntOrNull() ?: 0
            urlSuffix = m.groupValues[3]
        } else {
            baseUrl   = url
            lastOctet = -1
            urlSuffix = ""
        }
    }

    /**
     * Process one frame of controller input (called on the render thread after
     * nativeRenderFrame returns, while the EGL context is current for GL calls).
     */
    private fun processInput(menuEdge: Boolean, triggerEdge: Boolean, stickX: Float) {
        // Y button → toggle panel
        if (menuEdge) {
            panelVisible = !panelVisible
            if (panelVisible) {
                uploadPanelBitmap()
                Log.i(TAG, "VR panel shown (url=$baseUrl$lastOctet$urlSuffix)")
            } else {
                nativeSetPanelTexture(0)
                if (panelTexId != 0) {
                    GLES30.glDeleteTextures(1, intArrayOf(panelTexId), 0)
                    panelTexId = 0
                }
                Log.i(TAG, "VR panel hidden")
            }
        }

        if (!panelVisible) return

        // Left thumbstick X → scroll last IP octet (auto-repeat every 200 ms)
        if (lastOctet >= 0 && Math.abs(stickX) > 0.5f) {
            val now = System.currentTimeMillis()
            if (now - lastOctetChangeMs > 200L) {
                lastOctetChangeMs = now
                lastOctet = (lastOctet + if (stickX > 0f) 1 else -1).coerceIn(0, 255)
                uploadPanelBitmap()
            }
        } else if (Math.abs(stickX) < 0.2f) {
            lastOctetChangeMs = 0L   // reset debounce when stick returns to centre
        }

        // A button → confirm and connect
        if (triggerEdge) {
            val url = if (lastOctet >= 0) "$baseUrl$lastOctet$urlSuffix" else baseUrl
            Log.i(TAG, "VR panel confirmed: $url")
            panelVisible = false
            nativeSetPanelTexture(0)
            if (panelTexId != 0) {
                GLES30.glDeleteTextures(1, intArrayOf(panelTexId), 0)
                panelTexId = 0
            }
            activity.runOnUiThread { onConnectRequested?.invoke(url) }
        }
    }

    /**
     * Draw the connection panel Bitmap and upload it to the GL texture.
     * Must be called on the render thread (EGL context current).
     */
    private fun uploadPanelBitmap() {
        val pw = PANEL_W.toFloat()
        val ph = PANEL_H.toFloat()
        val bmp = Bitmap.createBitmap(PANEL_W, PANEL_H, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bmp)

        // Dark semi-transparent background with blue border
        val bgPaint = Paint().apply { color = Color.argb(220, 0, 20, 40) }
        canvas.drawRoundRect(RectF(0f, 0f, pw, ph), 12f, 12f, bgPaint)
        val borderPaint = Paint().apply {
            color = Color.argb(255, 0, 120, 200)
            style = Paint.Style.STROKE
            strokeWidth = 2f
        }
        canvas.drawRoundRect(RectF(1f, 1f, pw - 1f, ph - 1f), 11f, 11f, borderPaint)

        val mono = Typeface.MONOSPACE

        // Title
        val titlePaint = Paint().apply {
            color = Color.WHITE
            textSize = 26f
            isAntiAlias = true
            typeface = Typeface.create(mono, Typeface.BOLD)
            textAlign = Paint.Align.CENTER
        }
        canvas.drawText("VR CONNECT", pw / 2f, 34f, titlePaint)

        // URL with the editable last-octet highlighted in yellow
        val urlPaint = Paint().apply {
            color = Color.WHITE
            textSize = 20f
            isAntiAlias = true
            typeface = mono
        }
        val hlPaint = Paint().apply {
            color = Color.YELLOW
            textSize = 20f
            isAntiAlias = true
            typeface = mono
        }
        val prefix   = baseUrl.ifEmpty { "?" }
        val octetStr = if (lastOctet >= 0) "$lastOctet" else ""
        val suffix   = urlSuffix
        val totalW   = urlPaint.measureText(prefix) +
                       hlPaint.measureText(octetStr) +
                       urlPaint.measureText(suffix)
        var x = (pw - totalW) / 2f
        val yUrl = 76f
        canvas.drawText(prefix,   x, yUrl, urlPaint); x += urlPaint.measureText(prefix)
        canvas.drawText(octetStr, x, yUrl, hlPaint);  x += hlPaint.measureText(octetStr)
        canvas.drawText(suffix,   x, yUrl, urlPaint)

        // Controller hints
        val hintPaint = Paint().apply {
            color = Color.argb(200, 150, 200, 255)
            textSize = 15f
            isAntiAlias = true
            typeface = mono
            textAlign = Paint.Align.CENTER
        }
        canvas.drawText("Stick: change IP  |  A: connect  |  Y: close", pw / 2f, 118f, hintPaint)

        // Upload to GL texture (create on first use)
        if (panelTexId == 0) {
            val ids = IntArray(1)
            GLES30.glGenTextures(1, ids, 0)
            panelTexId = ids[0]
            GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, panelTexId)
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MIN_FILTER, GLES30.GL_LINEAR)
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MAG_FILTER, GLES30.GL_LINEAR)
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_S, GLES30.GL_CLAMP_TO_EDGE)
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_T, GLES30.GL_CLAMP_TO_EDGE)
            nativeSetPanelTexture(panelTexId)
            Log.i(TAG, "Created panel GL texture id=$panelTexId")
        }
        GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, panelTexId)
        GLUtils.texImage2D(GLES30.GL_TEXTURE_2D, 0, bmp, 0)
        bmp.recycle()
    }

    // ── JNI ───────────────────────────────────────────────────────────────────

    /**
     * Tell C++ which GL_TEXTURE_2D to sample for the stats overlay.
     * Maps to Java_com_fpv_quest_XrRenderThread_nativeSetStatsTexture in xr_renderer.cpp.
     */
    private external fun nativeSetStatsTexture(texId: Int)
    private external fun nativeSetPanelTexture(texId: Int)
    private external fun nativeSetStatusTexture(texId: Int)
    private external fun nativeGetLastInputState(out: FloatArray)

    // ── Lifecycle ─────────────────────────────────────────────────────────────

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
