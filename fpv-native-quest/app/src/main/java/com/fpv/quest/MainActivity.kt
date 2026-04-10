package com.fpv.quest

import android.app.Activity
import android.app.AlarmManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.net.wifi.WifiManager
import android.os.Bundle
import android.os.SystemClock
import android.util.Log
import android.view.View
import android.view.inputmethod.EditorInfo
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import org.webrtc.EglBase
import org.webrtc.RendererCommon
import org.webrtc.SurfaceViewRenderer

/**
 * Entry point for the FPV Quest native app.
 *
 * Flat (non-VR) mode: video renders on SurfaceViewRenderer, status in a text strip.
 * VR mode: OpenXR stereo rendering via XrRenderThread (TASK-005).
 *
 * The server URL is persisted in SharedPreferences so it survives restarts.
 *
 * TASK-003: SignalingClient + WebRTCEngine + flat SurfaceViewRenderer.
 * TASK-004: EglVideoSink + zero-copy OES texture path wired to native code.
 * TASK-005: XrRenderThread + OpenXR stereo rendering; DataChannel E2E stats.
 */
class MainActivity : Activity() {

    companion object {
        private const val TAG = "FPVQuest"
        private const val PREFS_NAME = "fpv"
        private const val PREF_SERVER_URL = "server_url"
        private const val DEFAULT_URL = "ws://192.168.x.x:8080"
        // AlarmManager request code for the compositor-transition auto-restart alarm.
        private const val ALARM_RESTART_RC = 42

        init {
            System.loadLibrary("fpv-native")
        }

        // ── JNI — video_decoder.cpp ────────────────────────────────────────────

        /** Allocate a placeholder GL_TEXTURE_EXTERNAL_OES texture; returns its GL name. */
        @JvmStatic external fun nativeCreateVideoTexture(): Int

        /**
         * Store the latest WebRTC OES texture ID and SurfaceTexture transform matrix.
         * Called by EglVideoSink on every hardware-decoded frame.
         */
        @JvmStatic external fun nativeUpdateVideoFrame(textureId: Int, transformMatrix: FloatArray)

        /** Return the most recently stored OES texture ID (0 if no frame yet). */
        @JvmStatic external fun nativeGetVideoTextureId(): Int

        /** Reset the stored video texture ID to 0 (called on teardown so XR shows status overlay). */
        @JvmStatic external fun nativeResetVideoState()

        /** Copy the most recently stored 3×3 transform matrix (float[9], row-major). */
        @JvmStatic external fun nativeGetVideoTransformMatrix(outMatrix: FloatArray)

        // ── JNI — xr_renderer.cpp (TASK-005) ──────────────────────────────────

        /**
         * Initialize OpenXR instance, session, swapchains, and GL shaders.
         * Must be called from XrRenderThread with a shared EGL context current.
         * @param activity  The MainActivity instance (for XrInstanceCreateInfoAndroidKHR).
         * @return true on success.
         */
        @JvmStatic external fun nativeInitXR(activity: Activity): Boolean

        /**
         * Render one stereo frame.
         * Blocks on xrWaitFrame() for natural pacing (~13.9 ms at 72 Hz).
         * @return true  keep running; false  session exiting (stop the render loop).
         */
        @JvmStatic external fun nativeRenderFrame(): Boolean

        /**
         * Request the XR session to exit, unblocking a pending xrWaitFrame().
         * Safe to call even if the session is not running.
         */
        @JvmStatic external fun nativeRequestExitSession()

        /** Destroy the XR session and release all native resources. */
        @JvmStatic external fun nativeDestroyXR()

        /**
         * Returns true if the XR session reached XR_SESSION_STATE_FOCUSED at least once
         * since the last nativeInitXR() call.  Used to distinguish a compositor-transition
         * kill (never focused → auto-restart) from a deliberate user exit.
         */
        @JvmStatic external fun nativeWasSessionFocused(): Boolean
    }

    // EGL context shared between HardwareVideoDecoderFactory, SurfaceViewRenderer,
    // and XrRenderThread (via EglBase.create(eglBase.eglBaseContext)).
    private lateinit var eglBase: EglBase

    // Views
    private lateinit var renderer: SurfaceViewRenderer
    private lateinit var overlay: View
    private lateinit var urlInput: EditText
    private lateinit var connectButton: Button
    private lateinit var statusText: TextView

    // WebRTC stack
    private var engine: WebRTCEngine? = null
    private var signaling: SignalingClient? = null
    private var eglVideoSink: EglVideoSink? = null

    // OpenXR render thread (TASK-005)
    // @Volatile: read from WebRTC IO thread (onTimestamp callback), written on main thread.
    @Volatile private var xrThread: XrRenderThread? = null

    // TASK-007: WiFi High Performance Lock — prevents the WiFi driver from entering
    // power-save mode while streaming.  Power-save mode adds 10–50 ms latency spikes
    // because the driver buffers packets during sleep intervals (typically 100 ms DTIM).
    // Held from startConnection() through teardown()/onStop().
    private var wifiLock: WifiManager.WifiLock? = null

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        Log.i(TAG, "onCreate")

        // 1. EGL context — shared between decoder, renderer, and XrRenderThread
        eglBase = EglBase.create()

        // 2. Video renderer (flat mode — also active in VR, Quest compositor ignores it)
        renderer = findViewById(R.id.surface_view)
        renderer.init(eglBase.eglBaseContext, null)
        renderer.setScalingType(RendererCommon.ScalingType.SCALE_ASPECT_FIT)
        renderer.setMirror(false)

        // 3. UI views
        overlay       = findViewById(R.id.overlay)
        urlInput      = findViewById(R.id.url_input)
        connectButton = findViewById(R.id.connect_button)
        statusText    = findViewById(R.id.status_text)

        // 4. Restore last-used URL
        val prefs = getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        urlInput.setText(prefs.getString(PREF_SERVER_URL, DEFAULT_URL))

        // 5. Connect button
        connectButton.setOnClickListener { startConnection() }

        urlInput.setOnEditorActionListener { _, actionId, _ ->
            if (actionId == EditorInfo.IME_ACTION_DONE) {
                startConnection()
                true
            } else false
        }
    }

    override fun onResume() {
        super.onResume()
        Log.d(TAG, "onResume")
        // Cancel any pending auto-restart alarm — we're launching normally now.
        cancelAutoRestartAlarm()
        // With com.oculus.intent.category.VR, Quest expects the XR session to start
        // immediately — otherwise it shows its loading spinner indefinitely.
        startXrThread()

        // Once the XR session is FOCUSED the 2D overlay is hidden by the VR compositor,
        // so auto-connect to the last-saved URL on launch.  The overlay stays as a
        // fallback (visible as a VR panel before FOCUSED, or after a failed connection).
        if (engine == null) {
            val url = urlInput.text.toString().trim()
            if ((url.startsWith("ws://") || url.startsWith("wss://")) && !url.contains("x.x")) {
                overlay.visibility = View.GONE
                Log.i(TAG, "Auto-connecting to saved URL: $url")
                startConnection()
            }
        }
    }

    override fun onPause() {
        super.onPause()
        Log.d(TAG, "onPause")
        // Do NOT stop XrRenderThread here. With com.oculus.intent.category.VR,
        // onPause fires when the Quest system menu opens — the XR session is still
        // running (VISIBLE state). The XR state machine handles this naturally.

        // Compositor-transition auto-restart:
        //
        // When the user returns from Home and re-launches the app, Quest OS sometimes
        // performs a VR-compositor transition (VR→shellenv→VR) and force-finishes the
        // Activity before the XR session reaches FOCUSED.  The process is killed ~30 ms
        // later.  The user then has to tap the icon a second time to get a working session.
        //
        // Detection: isFinishing() is true (OS force-finish) AND the session never reached
        // XR_SESSION_STATE_FOCUSED (nativeWasSessionFocused() == false).
        //
        // Fix: schedule an AlarmManager intent ~2 s in the future.  AlarmManager lives in
        // system_server and fires even after our process is dead, restarting the Activity
        // automatically so the user doesn't need to tap twice.
        if (isFinishing && xrThread != null && !nativeWasSessionFocused()) {
            scheduleAutoRestartAlarm()
        }
    }

    override fun onStop() {
        super.onStop()
        Log.d(TAG, "onStop")
        // Do NOT stop XrRenderThread here. The XR session continues in the
        // background; the runtime sends XR_SESSION_STATE_STOPPING when it wants
        // us to stop, causing nativeRenderFrame() to return false on its own.
        releaseWifiLock()   // TASK-007: release on stop so the radio can sleep when backgrounded
    }

    override fun onDestroy() {
        super.onDestroy()
        Log.d(TAG, "onDestroy")
        stopXrThread()
        teardown()
        renderer.release()   // must precede eglBase.release()
        eglBase.release()

        // Quest OS force-kills Activity instances that launch in a "warm" process
        // (i.e. a process that already had an XR session).  Fresh processes always
        // succeed.  Killing ourselves here guarantees the next launch (manual tap
        // or the AlarmManager auto-restart) gets a clean Zygote fork instead of
        // reusing our process via singleTask.
        Log.i(TAG, "onDestroy: terminating process so next launch is always fresh")
        android.os.Process.killProcess(android.os.Process.myPid())
    }

    // ── OpenXR render thread management ─────────────────────────────────────

    private fun startXrThread() {
        if (xrThread != null) return
        val savedUrl = getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getString(PREF_SERVER_URL, DEFAULT_URL) ?: DEFAULT_URL
        xrThread = XrRenderThread(
            activity           = this,
            webRtcEglContext   = eglBase.eglBaseContext,
            serverUrl          = savedUrl,
            onConnectRequested = { url ->
                // Called on main thread when user confirms URL via VR panel (A button)
                urlInput.setText(url)
                startConnection()
            }
        ).also { it.start() }
        Log.i(TAG, "XrRenderThread started (url=$savedUrl)")
    }

    private fun stopXrThread() {
        xrThread?.let { t ->
            t.stopRendering()
            t.join(3000)
            if (t.isAlive) Log.w(TAG, "XrRenderThread did not stop within 3 s")
        }
        xrThread = null
    }

    // ── Connection management ─────────────────────────────────────────────────

    private fun startConnection() {
        val url = urlInput.text.toString().trim()
        if (!url.startsWith("ws://") && !url.startsWith("wss://")) {
            setStatus("Invalid URL — must start with ws:// or wss://")
            return
        }

        // Persist the URL and sync it to the VR panel
        getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(PREF_SERVER_URL, url)
            .apply()
        xrThread?.setServerUrl(url)

        overlay.visibility = View.GONE
        setStatus("Connecting…")

        // TASK-007: acquire WiFi High Performance Lock before tearing down so it stays
        // held across reconnects without a gap.  acquireWifiLock() is idempotent.
        acquireWifiLock()

        // Tear down any previous session before creating a new one
        teardown()
        xrThread?.clearStats()                // hide stale E2E HUD from previous session
        xrThread?.showStatus("Connecting…")   // show in VR while waiting for stream

        // Init the WebRTC stack.
        engine = WebRTCEngine()
        engine!!.init(this, eglBase)

        // Zero-copy OES sink (TASK-004): receives TextureBuffer frames from the
        // hardware decoder and forwards texture ID + transform matrix to native code.
        eglVideoSink = EglVideoSink { texId, matrix ->
            nativeUpdateVideoFrame(texId, matrix)
        }

        // Wire DataChannel callbacks for clock sync + E2E stats display
        engine!!.dataChannel.onClockSynced = { offsetMs ->
            Log.i(TAG, "Clock sync complete: offset=${offsetMs}ms")
        }
        engine!!.dataChannel.onTimestamp = { capture, encode ->
            val e2eMs = engine!!.dataChannel.computeE2E(capture)
            val encMs = encode - capture
            Log.i(TAG, "onTimestamp: e2e=${e2eMs}ms enc=${encMs}ms xrThread=${xrThread != null}")
            xrThread?.showStatus(null)            // video is flowing — hide status overlay
            xrThread?.updateStats(e2eMs, encMs)   // VR HUD overlay (TASK-006)
            runOnUiThread { setStatus("E2E: ${e2eMs}ms  enc: ${encMs}ms") }
        }
        engine!!.dataChannel.onClosed = {
            xrThread?.clearStats()
            xrThread?.showStatus("No stream — DataChannel closed")
            runOnUiThread { setStatus("DataChannel closed") }
        }

        // Build SignalingClient with callbacks that delegate to the engine
        signaling = SignalingClient(
            onOffer = { sdp ->
                engine?.handleOffer(sdp, signaling!!)
            },
            onIce = { candidate, sdpMLineIndex, sdpMid ->
                engine?.handleRemoteIce(candidate, sdpMLineIndex, sdpMid)
            },
            onDisconnected = {
                xrThread?.showStatus("No signal — check server address")
                runOnUiThread {
                    setStatus("Disconnected — check server and reconnect")
                    overlay.visibility = View.VISIBLE
                }
            }
        )

        engine!!.start(
            signaling    = signaling!!,
            videoSink    = renderer,
            eglVideoSink = eglVideoSink,
            onStatus     = { state ->
                runOnUiThread {
                    setStatus(state)
                    when (state) {
                        "connected" -> {
                            // Video will appear automatically in XR swapchain once
                            // nativeUpdateVideoFrame receives frames from EglVideoSink.
                            xrThread?.showStatus("Connected — waiting for video…")
                        }
                        "disconnected" -> {
                            // Transient ICE state — do NOT kill the XR session.
                            xrThread?.showStatus("Reconnecting…")
                        }
                        "failed" -> {
                            xrThread?.showStatus("Connection failed — check IP/server")
                            overlay.visibility = View.VISIBLE
                        }
                        "closed" -> {
                            xrThread?.showStatus("Stream closed")
                            overlay.visibility = View.VISIBLE
                        }
                    }
                }
            }
        )

        signaling!!.connect(url)
        Log.i(TAG, "Connecting to $url")
    }

    private fun teardown() {
        signaling?.close()
        signaling = null
        engine?.close()
        engine = null
        eglVideoSink = null
        // Reset C++ video state so hasVideo=false and the XR status overlay can appear
        // while waiting for a new stream.
        nativeResetVideoState()
        // TASK-007: WiFi lock is NOT released here — it stays held between reconnects
        // to avoid power-save latency spikes during the signaling/ICE phase.
        // It is released in onStop() when the app goes to background.
    }

    // ── TASK-007: WiFi High Performance Lock helpers ─────────────────────────────

    @Suppress("DEPRECATION") // WIFI_MODE_FULL_HIGH_PERF is deprecated in API 29 but still works
    private fun acquireWifiLock() {
        if (wifiLock?.isHeld == true) return
        val wm = applicationContext.getSystemService(WIFI_SERVICE) as? WifiManager ?: return
        wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "fpv_wifi_lock").also {
            it.setReferenceCounted(false)
            it.acquire()
            Log.i(TAG, "WiFi High Performance Lock acquired")
        }
    }

    private fun releaseWifiLock() {
        wifiLock?.let { lock ->
            if (lock.isHeld) {
                lock.release()
                Log.i(TAG, "WiFi High Performance Lock released")
            }
        }
        wifiLock = null
    }

    // ── Compositor-transition auto-restart helpers ────────────────────────────────

    private fun makeRestartIntent(): PendingIntent = PendingIntent.getActivity(
        applicationContext,
        ALARM_RESTART_RC,
        Intent(applicationContext, MainActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP),
        PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
    )

    private fun scheduleAutoRestartAlarm() {
        val am = getSystemService(AlarmManager::class.java) ?: return
        am.set(
            AlarmManager.ELAPSED_REALTIME,
            SystemClock.elapsedRealtime() + 2_000L,
            makeRestartIntent()
        )
        Log.i(TAG, "Auto-restart alarm scheduled in 2 s (compositor transition recovery)")
    }

    private fun cancelAutoRestartAlarm() {
        val am = getSystemService(AlarmManager::class.java) ?: return
        am.cancel(makeRestartIntent())
    }

    private fun setStatus(msg: String) {
        statusText.text = msg
    }
}
