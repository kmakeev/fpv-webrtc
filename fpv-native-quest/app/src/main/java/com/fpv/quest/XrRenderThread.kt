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

    // Regex: ws(s)://A.B.C.D[:port]  — captures all 4 octets separately
    private val urlRegex = Regex("""^(wss?://)(\d+)\.(\d+)\.(\d+)\.(\d+)(:\d+)?$""")

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
    private var panelVisible       = false
    private var urlScheme          = "ws://"   // "ws://" or "wss://"
    private val octets             = IntArray(4)  // four IP octets, each 0..255
    private var urlPort            = ""           // e.g. ":8080" or ""
    private var isParseable        = false        // false if URL doesn't match expected pattern
    private var rawUrl             = ""           // kept as-is when not parseable
    private var currentOctetIdx    = 3            // which octet is currently selected (0..3)
    private var panelTexId         = 0            // GL texture name; 0 = not yet created
    private var stickXChangeMs     = 0L           // debounce timer for X-axis (change value)
    private var stickYChangeMs     = 0L           // debounce timer for Y-axis (navigate octet)
    private val inputBuf           = FloatArray(4) // reused per-frame for nativeGetLastInputState

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
                processInput(inputBuf[0] > 0.5f, inputBuf[1] > 0.5f, inputBuf[2], inputBuf[3])
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
            isParseable = true
            urlScheme = m.groupValues[1]
            for (i in 0..3) octets[i] = m.groupValues[2 + i].toIntOrNull()?.coerceIn(0, 255) ?: 0
            urlPort = m.groupValues[6]
        } else {
            isParseable = false
            rawUrl = url
        }
    }

    private fun buildUrl(): String = if (isParseable)
        "$urlScheme${octets[0]}.${octets[1]}.${octets[2]}.${octets[3]}$urlPort"
    else rawUrl

    /**
     * Process one frame of controller input (called on the render thread after
     * nativeRenderFrame returns, while the EGL context is current for GL calls).
     *
     * Left thumbstick X → change value of currently selected octet (0–255)
     * Left thumbstick Y → navigate between octets (left = prev, right = next)
     * Y button          → toggle panel
     * A button          → confirm and connect
     */
    private fun processInput(menuEdge: Boolean, triggerEdge: Boolean,
                             stickX: Float, stickY: Float) {
        // Y button → toggle panel
        if (menuEdge) {
            panelVisible = !panelVisible
            if (panelVisible) {
                uploadPanelBitmap()
                Log.i(TAG, "VR panel shown (url=${buildUrl()})")
            } else {
                hidePanelTexture()
                Log.i(TAG, "VR panel hidden")
            }
        }

        if (!panelVisible || !isParseable) return

        val now = System.currentTimeMillis()

        // Left thumbstick X → change value of current octet
        if (Math.abs(stickX) > 0.5f) {
            val delay = if (stickXChangeMs == 0L) 400L else 150L   // initial delay then repeat
            if (now - stickXChangeMs > delay) {
                stickXChangeMs = now
                octets[currentOctetIdx] = (octets[currentOctetIdx] + if (stickX > 0f) 1 else -1)
                    .coerceIn(0, 255)
                uploadPanelBitmap()
            }
        } else if (Math.abs(stickX) < 0.2f) {
            stickXChangeMs = 0L
        }

        // Left thumbstick Y → navigate between octets
        // Push up (Y > 0.5) = move to previous octet (lower index)
        // Push down (Y < -0.5) = move to next octet (higher index)
        if (Math.abs(stickY) > 0.5f) {
            if (now - stickYChangeMs > 400L) {
                stickYChangeMs = now
                currentOctetIdx = (currentOctetIdx + if (stickY > 0f) -1 else 1).coerceIn(0, 3)
                uploadPanelBitmap()
            }
        } else if (Math.abs(stickY) < 0.2f) {
            stickYChangeMs = 0L
        }

        // A button → confirm and connect
        if (triggerEdge) {
            val url = buildUrl()
            Log.i(TAG, "VR panel confirmed: $url")
            panelVisible = false
            hidePanelTexture()
            activity.runOnUiThread { onConnectRequested?.invoke(url) }
        }
    }

    private fun hidePanelTexture() {
        nativeSetPanelTexture(0)
        if (panelTexId != 0) {
            GLES30.glDeleteTextures(1, intArrayOf(panelTexId), 0)
            panelTexId = 0
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

        val urlPaint = Paint().apply {
            color = Color.WHITE
            textSize = 22f
            isAntiAlias = true
            typeface = mono
        }
        val hlPaint = Paint().apply {
            color = Color.YELLOW
            textSize = 22f
            isAntiAlias = true
            typeface = mono
        }
        val dotPaint = Paint(urlPaint)

        if (isParseable) {
            // Build the list of segments: scheme, oct0, ".", oct1, ".", oct2, ".", oct3, port
            // Selected octet uses hlPaint; everything else uses urlPaint.
            data class Segment(val text: String, val paint: Paint)
            val segments = mutableListOf<Segment>()
            segments += Segment(urlScheme, urlPaint)
            for (i in 0..3) {
                segments += Segment(octets[i].toString(), if (i == currentOctetIdx) hlPaint else urlPaint)
                if (i < 3) segments += Segment(".", dotPaint)
            }
            segments += Segment(urlPort, urlPaint)

            val totalW = segments.sumOf { it.paint.measureText(it.text).toDouble() }.toFloat()
            var x = (pw - totalW) / 2f
            val yUrl = 82f
            for (seg in segments) {
                canvas.drawText(seg.text, x, yUrl, seg.paint)
                x += seg.paint.measureText(seg.text)
            }

            // Underline below the selected octet to make cursor obvious
            val underlinePaint = Paint().apply {
                color = Color.YELLOW
                strokeWidth = 2f
                style = Paint.Style.STROKE
            }
            // Recalculate x position of selected octet
            var ux = (pw - totalW) / 2f
            for (seg in segments) {
                val w = seg.paint.measureText(seg.text)
                if (seg.paint === hlPaint) {
                    canvas.drawLine(ux, yUrl + 4f, ux + w, yUrl + 4f, underlinePaint)
                    break
                }
                ux += w
            }
        } else {
            // Not parseable: show raw URL centred, no editing
            urlPaint.textAlign = Paint.Align.CENTER
            canvas.drawText(rawUrl.take(44), pw / 2f, 82f, urlPaint)
            val notePaint = Paint().apply {
                color = Color.argb(180, 255, 180, 100)
                textSize = 15f
                isAntiAlias = true
                typeface = mono
                textAlign = Paint.Align.CENTER
            }
            canvas.drawText("URL format not recognised — edit manually", pw / 2f, 106f, notePaint)
        }

        // Controller hints
        val hintPaint = Paint().apply {
            color = Color.argb(200, 150, 200, 255)
            textSize = 14f
            isAntiAlias = true
            typeface = mono
            textAlign = Paint.Align.CENTER
        }
        canvas.drawText("← → change value  ↑ ↓ select octet  A: connect  Y: close",
                        pw / 2f, 140f, hintPaint)

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
