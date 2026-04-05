package com.fpv.quest

import android.content.Context
import android.util.Log
import org.webrtc.DataChannel
import org.webrtc.EglBase
import org.webrtc.HardwareVideoDecoderFactory
import org.webrtc.IceCandidate
import org.webrtc.MediaConstraints
import org.webrtc.MediaStream
import org.webrtc.MediaStreamTrack
import org.webrtc.PeerConnection
import org.webrtc.PeerConnectionFactory
import org.webrtc.RtpReceiver
import org.webrtc.RtpTransceiver
import org.webrtc.SdpObserver
import org.webrtc.SessionDescription
import org.webrtc.VideoSink
import org.webrtc.VideoTrack

/**
 * WebRTC peer connection engine — Kotlin equivalent of public/js/webrtc-client.js.
 *
 * Responsibilities:
 *   - Initialize PeerConnectionFactory with HardwareVideoDecoderFactory (MediaCodec)
 *   - Create RTCPeerConnection with STUN config (UNIFIED_PLAN)
 *   - Handle offer/answer/ICE exchange via SignalingClient
 *   - Reorder SDP to prefer H.264 in the answer (same logic as webrtc-client.js reorderH264)
 *   - Add VideoTrack sink for rendering (SurfaceViewRenderer)
 *   - Wire incoming DataChannel into FPVDataChannel
 */
class WebRTCEngine {

    companion object {
        private const val TAG = "WebRTCEngine"
        private const val STUN_SERVER = "stun:stun.l.google.com:19302"
    }

    private var factory: PeerConnectionFactory? = null
    private var pc: PeerConnection? = null
    private var videoTrackRef: VideoTrack? = null
    private var videoSinkRef: VideoSink? = null

    /** Owns the FPVDataChannel instance; bound once the peer sends a DataChannel. */
    val dataChannel = FPVDataChannel()

    // ── Lifecycle ────────────────────────────────────────────────────────────

    /**
     * Initialize PeerConnectionFactory with HardwareVideoDecoderFactory.
     * Must be called before start(). eglBase is shared with SurfaceViewRenderer.
     *
     * PeerConnectionFactory.initialize() is idempotent — safe to call multiple times.
     */
    fun init(context: Context, eglBase: EglBase) {
        PeerConnectionFactory.initialize(
            PeerConnectionFactory.InitializationOptions.builder(context)
                .createInitializationOptions()
        )
        // HardwareVideoDecoderFactory uses MediaCodec + the shared EGL context so the
        // decoded frame goes directly to the GPU without a CPU copy.
        // Do NOT use DefaultVideoDecoderFactory — it may select a software decoder.
        val decoderFactory = HardwareVideoDecoderFactory(eglBase.eglBaseContext)

        factory = PeerConnectionFactory.builder()
            .setVideoDecoderFactory(decoderFactory)
            // No encoder factory — this is a receive-only viewer.
            .createPeerConnectionFactory()

        Log.i(TAG, "PeerConnectionFactory initialized (HardwareVideoDecoder)")
    }

    /**
     * Create the RTCPeerConnection and wire all callbacks.
     * Must be called after init().
     *
     * @param signaling  used to send answer and local ICE candidates
     * @param videoSink  SurfaceViewRenderer (or any VideoSink) that receives decoded frames
     * @param onStatus   called on connection state changes (arrives on WebRTC thread — dispatch to UI yourself)
     */
    fun start(
        signaling: SignalingClient,
        videoSink: VideoSink,
        onStatus: (String) -> Unit
    ) {
        videoSinkRef = videoSink

        val iceServers = listOf(
            PeerConnection.IceServer.builder(STUN_SERVER).createIceServer()
        )
        val config = PeerConnection.RTCConfiguration(iceServers).apply {
            sdpSemantics              = PeerConnection.SdpSemantics.UNIFIED_PLAN
            continualGatheringPolicy  = PeerConnection.ContinualGatheringPolicy.GATHER_CONTINUALLY
            bundlePolicy              = PeerConnection.BundlePolicy.MAXBUNDLE
            rtcpMuxPolicy             = PeerConnection.RtcpMuxPolicy.REQUIRE
        }

        pc = factory!!.createPeerConnection(config, object : PeerConnection.Observer {

            // ── ICE ──────────────────────────────────────────────────────────

            override fun onIceCandidate(candidate: IceCandidate) {
                Log.d(TAG, "local ICE: ${candidate.sdpMid} ${candidate.sdp.take(40)}")
                signaling.sendIce(candidate.sdp, candidate.sdpMLineIndex, candidate.sdpMid)
            }

            override fun onIceCandidatesRemoved(candidates: Array<out IceCandidate>) {
                Log.d(TAG, "ICE candidates removed: ${candidates.size}")
            }

            override fun onIceConnectionChange(state: PeerConnection.IceConnectionState) {
                Log.d(TAG, "ICE connection: $state")
            }

            override fun onIceConnectionReceivingChange(receiving: Boolean) {
                Log.d(TAG, "ICE receiving: $receiving")
            }

            override fun onIceGatheringChange(state: PeerConnection.IceGatheringState) {
                Log.d(TAG, "ICE gathering: $state")
            }

            // ── Tracks (UNIFIED_PLAN — use onTrack, not onAddStream) ─────────

            override fun onTrack(transceiver: RtpTransceiver) {
                val track = transceiver.receiver.track() ?: return
                if (track.kind() == MediaStreamTrack.VIDEO_TRACK_KIND) {
                    videoTrackRef = track as VideoTrack
                    videoTrackRef!!.addSink(videoSink)
                    Log.i(TAG, "VideoTrack attached to sink")
                }
            }

            override fun onAddTrack(receiver: RtpReceiver, streams: Array<out MediaStream>) {
                Log.d(TAG, "onAddTrack (PLAN_B legacy, ignored in UNIFIED_PLAN)")
            }

            override fun onAddStream(stream: MediaStream) {
                Log.d(TAG, "onAddStream (deprecated, ignored in UNIFIED_PLAN)")
            }

            override fun onRemoveStream(stream: MediaStream) {
                Log.d(TAG, "onRemoveStream")
            }

            // ── DataChannel ───────────────────────────────────────────────────

            override fun onDataChannel(dc: DataChannel) {
                Log.i(TAG, "DataChannel received: ${dc.label()}")
                dataChannel.bind(dc)
            }

            // ── Connection state ──────────────────────────────────────────────

            override fun onConnectionChange(newState: PeerConnection.PeerConnectionState) {
                Log.i(TAG, "connection: $newState")
                onStatus(newState.name.lowercase())
            }

            override fun onSignalingChange(state: PeerConnection.SignalingState) {
                Log.d(TAG, "signaling: $state")
            }

            override fun onRenegotiationNeeded() {
                Log.d(TAG, "renegotiation needed")
            }
        })

        Log.i(TAG, "PeerConnection created")
    }

    // ── Signaling handlers ────────────────────────────────────────────────────

    /**
     * Process an incoming SDP offer from the streamer.
     *
     * Sequence (must be strictly ordered via callbacks):
     *   setRemoteDescription(offer)
     *     → createAnswer()
     *       → preferH264(answer)
     *         → setLocalDescription(answer)
     *           → signaling.sendAnswer(answer)
     */
    fun handleOffer(sdp: String, signaling: SignalingClient) {
        val offer = SessionDescription(SessionDescription.Type.OFFER, sdp)
        pc?.setRemoteDescription(
            sdpObserver("setRemote") {
                pc?.createAnswer(
                    sdpObserver("createAnswer") { answerSdp ->
                        val finalSdp = preferH264(answerSdp!!.description)
                        val answer = SessionDescription(SessionDescription.Type.ANSWER, finalSdp)
                        pc?.setLocalDescription(
                            sdpObserver("setLocal") {
                                signaling.sendAnswer(finalSdp)
                                Log.i(TAG, "answer sent")
                            },
                            answer
                        )
                    },
                    MediaConstraints()
                )
            },
            offer
        )
    }

    /**
     * Add a remote ICE candidate received from the signaling server.
     *
     * IceCandidate constructor: IceCandidate(sdpMid, sdpMLineIndex, sdp) — sdpMid is FIRST.
     */
    fun handleRemoteIce(candidate: String, sdpMLineIndex: Int, sdpMid: String) {
        pc?.addIceCandidate(IceCandidate(sdpMid, sdpMLineIndex, candidate))
    }

    fun close() {
        // Remove sink before disposing to prevent a crash in some SDK versions
        videoTrackRef?.removeSink(videoSinkRef)
        videoTrackRef = null
        videoSinkRef  = null

        pc?.dispose()
        pc = null

        factory?.dispose()
        factory = null

        Log.i(TAG, "closed")
    }

    // ── SDP helpers ───────────────────────────────────────────────────────────

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

    // ── Private utilities ─────────────────────────────────────────────────────

    /**
     * Concise SdpObserver factory.
     * onSuccess receives the created SessionDescription (createAnswer/createOffer)
     * or null (setLocalDescription/setRemoteDescription).
     */
    private fun sdpObserver(
        tag: String,
        onSuccess: (SessionDescription?) -> Unit
    ) = object : SdpObserver {
        override fun onCreateSuccess(sdp: SessionDescription?) = onSuccess(sdp)
        override fun onSetSuccess()                            = onSuccess(null)
        override fun onCreateFailure(error: String?) { Log.e(TAG, "$tag createFailure: $error") }
        override fun onSetFailure(error: String?)    { Log.e(TAG, "$tag setFailure: $error") }
    }
}
