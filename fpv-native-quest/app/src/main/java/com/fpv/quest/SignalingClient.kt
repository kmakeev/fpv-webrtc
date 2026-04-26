package com.fpv.quest

import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import org.json.JSONObject
import java.security.SecureRandom
import java.security.cert.X509Certificate
import java.util.concurrent.TimeUnit
import javax.net.ssl.SSLContext
import javax.net.ssl.TrustManager
import javax.net.ssl.X509TrustManager

/**
 * WebSocket signaling client — mirrors the protocol of server/server.js and
 * the JS implementation in public/js/webrtc-client.js.
 *
 * Reconnects automatically with exponential backoff [1s, 2s, 4s, 8s, 30s] on
 * connection failure or closure. Call close() to stop reconnecting.
 *
 * Message types (server.js protocol):
 *   role             viewer → server  {"type":"role","role":"viewer"}
 *   offer            server → viewer  {"type":"offer","sdp":"..."}
 *   answer           viewer → server  {"type":"answer","sdp":"..."}
 *   ice              both ways        {"type":"ice","candidate":"...","sdpMLineIndex":0,"sdpMid":"0"}
 *   viewer_ready     server → viewer  {"type":"viewer_ready"}
 *   peer_disconnected server → viewer {"type":"peer_disconnected"}
 */
class SignalingClient(
    private val onOffer: (sdp: String) -> Unit,
    private val onIce: (candidate: String, sdpMLineIndex: Int, sdpMid: String) -> Unit,
    private val onDisconnected: () -> Unit
) {

    companion object {
        private const val TAG = "SignalingClient"

        const val TYPE_ROLE = "role"
        const val TYPE_OFFER = "offer"
        const val TYPE_ANSWER = "answer"
        const val TYPE_ICE = "ice"
        const val TYPE_VIEWER_READY = "viewer_ready"
        const val TYPE_PEER_DISCONNECTED = "peer_disconnected"

        private val BACKOFF_MS = longArrayOf(1_000, 2_000, 4_000, 8_000, 30_000)
    }

    // Trust all TLS certificates — required for the self-signed LAN cert (Quest 2
    // has no UI to install user CA certs). Safe for a private LAN-only app.
    private val httpClient: OkHttpClient = buildTrustAllClient()

    private fun buildTrustAllClient(): OkHttpClient {
        val trustAll = arrayOf<TrustManager>(object : X509TrustManager {
            override fun checkClientTrusted(chain: Array<X509Certificate>, authType: String) {}
            override fun checkServerTrusted(chain: Array<X509Certificate>, authType: String) {}
            override fun getAcceptedIssuers(): Array<X509Certificate> = arrayOf()
        })
        val sslContext = SSLContext.getInstance("TLS").apply {
            init(null, trustAll, SecureRandom())
        }
        return OkHttpClient.Builder()
            .connectTimeout(10, TimeUnit.SECONDS)
            .readTimeout(0, TimeUnit.MILLISECONDS)  // no timeout for persistent WS
            .sslSocketFactory(sslContext.socketFactory, trustAll[0] as X509TrustManager)
            .hostnameVerifier { _, _ -> true }
            .build()
    }

    private var ws: WebSocket? = null
    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
    private var reconnectJob: Job? = null

    @Volatile private var reconnectEnabled = false
    @Volatile private var backoffIndex = 0
    @Volatile private var currentUrl = ""

    /** Called when a reconnect attempt is about to be scheduled. delayMs = wait time. */
    var onReconnecting: ((delayMs: Long) -> Unit)? = null

    /** Connect to the signaling server and register as "viewer". Enables auto-reconnect. */
    fun connect(url: String) {
        currentUrl = url
        reconnectEnabled = true
        backoffIndex = 0
        _connectWs()
    }

    private fun _connectWs() {
        Log.i(TAG, "Connecting to $currentUrl (backoffIndex=$backoffIndex)")
        val request = Request.Builder().url(currentUrl).build()
        ws = httpClient.newWebSocket(request, Listener())
    }

    private fun handleDisconnect() {
        onDisconnected()
        if (!reconnectEnabled) return
        val delay = BACKOFF_MS.getOrElse(backoffIndex) { 30_000L }
        if (backoffIndex < BACKOFF_MS.size - 1) backoffIndex++
        onReconnecting?.invoke(delay)
        reconnectJob = scope.launch {
            delay(delay)
            if (isActive && reconnectEnabled) _connectWs()
        }
    }

    /** Send the SDP answer produced by WebRTCEngine. */
    fun sendAnswer(sdp: String) {
        send(JSONObject().apply {
            put("type", TYPE_ANSWER)
            put("sdp", sdp)
        }.toString())
    }

    /** Forward a local ICE candidate to the remote peer. */
    fun sendIce(candidate: String, sdpMLineIndex: Int, sdpMid: String) {
        send(JSONObject().apply {
            put("type", TYPE_ICE)
            put("candidate", candidate)
            put("sdpMLineIndex", sdpMLineIndex)
            put("sdpMid", sdpMid)
        }.toString())
    }

    fun close() {
        reconnectEnabled = false
        reconnectJob?.cancel()
        reconnectJob = null
        scope.cancel()
        ws?.close(1000, "App closing")
        ws = null
    }

    private fun send(json: String) {
        if (ws?.send(json) == false) {
            Log.w(TAG, "WebSocket send failed — not connected?")
        }
    }

    private inner class Listener : WebSocketListener() {
        override fun onOpen(webSocket: WebSocket, response: Response) {
            Log.i(TAG, "Connected")
            backoffIndex = 0
            // Register as viewer — triggers streamer's viewer_ready
            send(JSONObject().apply {
                put("type", TYPE_ROLE)
                put("role", "viewer")
            }.toString())
        }

        override fun onMessage(webSocket: WebSocket, text: String) {
            try {
                val msg = JSONObject(text)
                when (msg.getString("type")) {
                    TYPE_OFFER -> {
                        Log.d(TAG, "offer received")
                        onOffer(msg.getString("sdp"))
                    }
                    TYPE_ICE -> {
                        Log.d(TAG, "ice candidate received")
                        // The streamer (Chrome) sends RTCIceCandidate serialised as a nested
                        // JSON object: { candidate: { candidate:"...", sdpMLineIndex:0, sdpMid:"0" } }
                        // The flat format { candidate:"...", sdpMLineIndex:0, sdpMid:"0" } is also
                        // supported for symmetry with what we send.
                        val rawCandidate = msg.get("candidate")
                        if (rawCandidate is org.json.JSONObject) {
                            onIce(
                                rawCandidate.optString("candidate", ""),
                                rawCandidate.optInt("sdpMLineIndex", 0),
                                rawCandidate.optString("sdpMid", "0")
                            )
                        } else {
                            onIce(
                                msg.optString("candidate", ""),
                                msg.optInt("sdpMLineIndex", 0),
                                msg.optString("sdpMid", "0")
                            )
                        }
                    }
                    TYPE_PEER_DISCONNECTED -> {
                        Log.i(TAG, "peer disconnected")
                        handleDisconnect()
                    }
                    TYPE_VIEWER_READY -> Log.d(TAG, "viewer_ready (no-op on viewer side)")
                    else -> Log.w(TAG, "Unknown message type: ${msg.optString("type")}")
                }
            } catch (e: Exception) {
                Log.e(TAG, "Error parsing message: $text", e)
            }
        }

        override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
            Log.e(TAG, "WebSocket failure: ${t.message}")
            handleDisconnect()
        }

        override fun onClosing(webSocket: WebSocket, code: Int, reason: String) {
            Log.i(TAG, "WebSocket closing: $code $reason")
            webSocket.close(1000, null)
            handleDisconnect()
        }
    }
}
