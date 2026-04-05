package com.fpv.quest

import android.content.Context
import android.util.Log

/**
 * WebRTC peer connection engine — Kotlin equivalent of public/js/webrtc-client.js.
 *
 * Responsibilities (when fully implemented in TASK-003):
 *   - Initialize PeerConnectionFactory with HardwareVideoDecoderFactory (MediaCodec)
 *   - Create RTCPeerConnection with STUN config
 *   - Handle offer/answer/ICE exchange via SignalingClient callbacks
 *   - Reorder SDP to prefer H.264 (same logic as webrtc-client.js reorderH264)
 *   - Expose VideoTrack for rendering (SurfaceTexture → xr_renderer)
 *   - Create DataChannel and hand it to FPVDataChannel
 *
 * TASK-002: stubs only — no actual WebRTC calls.
 * TODO TASK-003: implement with io.github.webrtc-sdk:android.
 */
class WebRTCEngine {

    companion object {
        private const val TAG = "WebRTCEngine"
        private const val STUN_SERVER = "stun:stun.l.google.com:19302"
    }

    fun init(context: Context) {
        Log.i(TAG, "init (TASK-002 stub)")
        // TODO TASK-003:
        // PeerConnectionFactory.initialize(
        //     PeerConnectionFactory.InitializationOptions.builder(context)
        //         .createInitializationOptions()
        // )
        // val eglBase = EglBase.create()
        // val decoderFactory = HardwareVideoDecoderFactory(eglBase.eglBaseContext)
        // val encoderFactory = CreateDefaultVideoEncoderFactory(eglBase.eglBaseContext, true, true)
        // factory = PeerConnectionFactory.builder()
        //     .setVideoDecoderFactory(decoderFactory)
        //     .setVideoEncoderFactory(encoderFactory)
        //     .createPeerConnectionFactory()
    }

    fun createPeerConnection() {
        Log.i(TAG, "createPeerConnection (TASK-002 stub)")
        // TODO TASK-003:
        // val iceServers = listOf(PeerConnection.IceServer.builder(STUN_SERVER).createIceServer())
        // val config = PeerConnection.RTCConfiguration(iceServers).apply {
        //     sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN
        //     continualGatheringPolicy = PeerConnection.ContinualGatheringPolicy.GATHER_CONTINUALLY
        // }
        // pc = factory.createPeerConnection(config, pcObserver)
    }

    /**
     * Reorders SDP m=video payload types to put H.264 first.
     *
     * Kotlin port of public/js/webrtc-client.js reorderH264().
     * Quest 2 has hardware H.264 decode (MediaCodec); putting it first
     * ensures it is selected over VP8/VP9.
     *
     * Example m=video line before:  "m=video 9 UDP/TLS/RTP/SAVPF 96 97 98 99 100 101"
     * After (H264 payload=96 first): "m=video 9 UDP/TLS/RTP/SAVPF 96 97 98 99 100 101"
     *                              → payload type 96 already first (noop) or reordered
     */
    fun preferH264(sdp: String): String {
        val lines = sdp.split("\r\n").toMutableList()
        val mVideoIdx = lines.indexOfFirst { it.startsWith("m=video") }
        if (mVideoIdx == -1) return sdp

        val nextMIdx = lines.drop(mVideoIdx + 1)
            .indexOfFirst { it.startsWith("m=") }
            .let { if (it == -1) lines.size else mVideoIdx + 1 + it }

        // Find H264 payload type from rtpmap lines in the video section
        val h264Payload = lines.subList(mVideoIdx + 1, nextMIdx)
            .firstOrNull { it.startsWith("a=rtpmap") && it.contains("H264", ignoreCase = true) }
            ?.let { Regex("a=rtpmap:(\\d+)").find(it)?.groupValues?.get(1) }
            ?: return sdp  // no H264 found, return unchanged

        // Reorder m=video payload list: "m=video PORT PROTO pt1 pt2 ..." → h264 first
        val parts = lines[mVideoIdx].split(" ").toMutableList()
        // parts[0]="m=video", parts[1]=port, parts[2]=proto, parts[3..]=payload types
        if (parts.size > 3 && parts.contains(h264Payload)) {
            parts.remove(h264Payload)
            parts.add(3, h264Payload)
            lines[mVideoIdx] = parts.joinToString(" ")
        }

        return lines.joinToString("\r\n")
    }

    fun close() {
        Log.i(TAG, "close")
        // TODO TASK-003: pc?.dispose(); factory?.dispose()
    }
}
