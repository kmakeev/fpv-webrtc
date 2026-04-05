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
import org.webrtc.PeerConnectionFactory
import org.webrtc.RendererCommon
import org.webrtc.SurfaceViewRenderer

/**
 * Entry point for the FPV Quest native app.
 *
 * Flat (non-VR) mode: video renders on SurfaceViewRenderer, status in a text strip.
 * The server URL is persisted in SharedPreferences so it survives restarts.
 *
 * TASK-003: SignalingClient + WebRTCEngine + flat SurfaceViewRenderer.
 * TODO TASK-004: initialize OpenXR session and xr_renderer for stereo display.
 * TODO TASK-005: wire dataChannel.onClockSynced / onTimestamp into the stats overlay.
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
    }

    // EGL context shared between HardwareVideoDecoderFactory and SurfaceViewRenderer
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

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        Log.i(TAG, "onCreate")

        // 1. EGL context — shared between decoder and renderer
        eglBase = EglBase.create()

        // 2. Video renderer
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

        // Allow "Done" on the keyboard to also trigger connect
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
    }

    override fun onPause() {
        super.onPause()
        Log.d(TAG, "onPause")
    }

    override fun onDestroy() {
        super.onDestroy()
        Log.d(TAG, "onDestroy")
        teardown()
        renderer.release()   // must precede eglBase.release()
        eglBase.release()
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

        // Init the WebRTC stack
        engine = WebRTCEngine()

        // PeerConnectionFactory.initialize() is idempotent
        PeerConnectionFactory.initialize(
            PeerConnectionFactory.InitializationOptions.builder(this)
                .createInitializationOptions()
        )
        engine!!.init(this, eglBase)

        // TODO TASK-005: wire clock sync and E2E timestamps into a stats overlay
        // engine!!.dataChannel.onClockSynced = { offsetMs -> ... }
        // engine!!.dataChannel.onTimestamp   = { capture, encode -> ... }

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
                    overlay.visibility = View.VISIBLE
                }
            }
        )

        engine!!.start(
            signaling  = signaling!!,
            videoSink  = renderer,
            onStatus   = { state -> runOnUiThread { setStatus(state) } }
        )

        signaling!!.connect(url)
        Log.i(TAG, "Connecting to $url")
    }

    private fun teardown() {
        signaling?.close()
        signaling = null
        engine?.close()
        engine = null
    }

    private fun setStatus(msg: String) {
        statusText.text = msg
    }
}
