package com.fpv.quest

import android.app.Activity
import android.content.Context
import android.os.Bundle
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
    private var xrThread: XrRenderThread? = null

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
    }

    override fun onStop() {
        super.onStop()
        Log.d(TAG, "onStop")
        // Do NOT stop XrRenderThread here. The XR session continues in the
        // background; the runtime sends XR_SESSION_STATE_STOPPING when it wants
        // us to stop, causing nativeRenderFrame() to return false on its own.
    }

    override fun onDestroy() {
        super.onDestroy()
        Log.d(TAG, "onDestroy")
        stopXrThread()
        teardown()
        renderer.release()   // must precede eglBase.release()
        eglBase.release()
    }

    // ── OpenXR render thread management ─────────────────────────────────────

    private fun startXrThread() {
        if (xrThread != null) return
        xrThread = XrRenderThread(this, eglBase.eglBaseContext).also { it.start() }
        Log.i(TAG, "XrRenderThread started")
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

        // Persist the URL
        getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(PREF_SERVER_URL, url)
            .apply()

        overlay.visibility = View.GONE
        setStatus("Connecting…")

        // Tear down any previous session before creating a new one
        teardown()

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
            runOnUiThread { setStatus("E2E: ${e2eMs}ms  enc: ${encMs}ms") }
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
                runOnUiThread {
                    setStatus("Disconnected — check server and reconnect")
                    // overlay shown via onStatus("disconnected") above
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
                        }
                        "disconnected" -> {
                            // Transient ICE state — do NOT kill the XR session.
                        }
                        "failed", "closed" -> {
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
    }

    private fun setStatus(msg: String) {
        statusText.text = msg
    }
}
