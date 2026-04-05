package com.fpv.quest

import android.util.Log
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import org.json.JSONObject
import java.util.concurrent.TimeUnit

/**
 * WebSocket signaling client — mirrors the protocol of server/server.js and
 * the JS implementation in public/js/webrtc-client.js.
 *
 * Message types (server.js protocol):
 *   role             viewer → server  {"type":"role","role":"viewer"}
 *   offer            server → viewer  {"type":"offer","sdp":"..."}
 *   answer           viewer → server  {"type":"answer","sdp":"..."}
 *   ice              both ways        {"type":"ice","candidate":"...","sdpMLineIndex":0,"sdpMid":"0"}
 *   viewer_ready     server → viewer  {"type":"viewer_ready"}
 *   peer_disconnected server → viewer {"type":"peer_disconnected"}
 *
 * TASK-002: constructor + connect() stub.
 * TODO TASK-003: wire callbacks into WebRTCEngine.
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
    }

    private val httpClient = OkHttpClient.Builder()
        .connectTimeout(10, TimeUnit.SECONDS)
        .readTimeout(0, TimeUnit.MILLISECONDS)  // no timeout for persistent WS
        .build()

    private var ws: WebSocket? = null

    /** Connect to the signaling server and register as "viewer". */
    fun connect(url: String) {
        Log.i(TAG, "Connecting to $url")
        val request = Request.Builder().url(url).build()
        ws = httpClient.newWebSocket(request, Listener())
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
                        onIce(
                            msg.getString("candidate"),
                            msg.getInt("sdpMLineIndex"),
                            msg.getString("sdpMid")
                        )
                    }
                    TYPE_PEER_DISCONNECTED -> {
                        Log.i(TAG, "peer disconnected")
                        onDisconnected()
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
            onDisconnected()
        }

        override fun onClosing(webSocket: WebSocket, code: Int, reason: String) {
            Log.i(TAG, "WebSocket closing: $code $reason")
            webSocket.close(1000, null)
            onDisconnected()
        }
    }
}
