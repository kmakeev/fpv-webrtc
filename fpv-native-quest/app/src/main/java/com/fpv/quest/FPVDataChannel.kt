package com.fpv.quest

import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.json.JSONObject
import org.webrtc.DataChannel
import java.nio.ByteBuffer
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CopyOnWriteArrayList

/**
 * DataChannel handler — Kotlin equivalent of public/js/datachannel.js.
 *
 * Protocol (channel name: "fpv", ordered, reliable):
 *   ping  viewer → streamer  {"type":"ping","id":N,"t0":timestamp}
 *   pong  streamer → viewer  {"type":"pong","id":N,"t0":timestamp,"t1":timestamp}
 *   ts    streamer → viewer  {"type":"ts","capture":timestamp,"encode":timestamp}
 *   head  viewer → streamer  reserved — future camera gimbal control
 *
 * Clock sync algorithm (mirrors datachannel.js syncClocks):
 *   1. Viewer sends 5 pings with t0 = System.currentTimeMillis()
 *   2. Streamer echoes pong with t0 and t1 = Date.now() (JS)
 *   3. Viewer computes offset = t1 - t0 - RTT/2 for each round-trip
 *   4. clockOffset = median of 5 measurements  (±1–2 ms on LAN)
 *
 * E2E latency calculation:
 *   e2eMs = System.currentTimeMillis() - capture + clockOffset
 *
 * Thread-safety notes:
 *   handleMessage/handlePong/handleTimestamp are called on the WebRTC internal thread.
 *   syncClocks runs on Dispatchers.IO.
 *   clockSynced and clockOffset are @Volatile for cross-thread visibility.
 *   pendingPings uses ConcurrentHashMap; collectedOffsets uses CopyOnWriteArrayList.
 */
class FPVDataChannel {

    companion object {
        private const val TAG = "FPVDataChannel"
        private const val SYNC_ROUNDS = 5
        private const val SYNC_STARTUP_DELAY_MS = 3_000L  // let jitter buffer stabilize
        private const val PING_INTERVAL_MS = 50L
    }

    private val scope = CoroutineScope(Dispatchers.IO)

    /** Offset: streamer clock − viewer clock (ms).  Positive → streamer ahead.
     *  @Volatile: written by IO coroutine, read by WebRTC message thread. */
    @Volatile private var clockOffset: Long = 0L

    /** @Volatile: written by IO coroutine, read by WebRTC message thread via handleTimestamp. */
    @Volatile private var clockSynced = false

    // ConcurrentHashMap: IO coroutine writes pendingPings[i], WebRTC thread reads/removes on pong.
    private val pendingPings = ConcurrentHashMap<Int, Long>()

    // CopyOnWriteArrayList: WebRTC thread appends on pong, IO coroutine reads after all pings done.
    private val collectedOffsets = CopyOnWriteArrayList<Long>()

    /** Called when clock sync completes. Param: offset in ms. */
    var onClockSynced: ((offsetMs: Long) -> Unit)? = null

    /** Called on each "ts" message from the streamer. */
    var onTimestamp: ((captureMs: Long, encodeMs: Long) -> Unit)? = null

    /** Called when the DataChannel enters CLOSING or CLOSED state. */
    var onClosed: (() -> Unit)? = null

    /** Send a JSON message over the DataChannel. Set by bind(). */
    @Volatile private var sendFn: ((String) -> Unit)? = null

    fun setSendFunction(fn: (String) -> Unit) {
        sendFn = fn
    }

    /**
     * Wire a real org.webrtc.DataChannel into this handler.
     * Called from WebRTCEngine.PeerConnection.Observer.onDataChannel().
     *
     * Mirrors datachannel.js _attachDc() — registers all callbacks and
     * starts clock sync once the channel reaches OPEN state.
     */
    fun bind(dc: DataChannel) {
        setSendFunction { json ->
            // ByteBuffer.wrap() produces a buffer already in read mode (position=0, limit=length)
            dc.send(DataChannel.Buffer(ByteBuffer.wrap(json.toByteArray(Charsets.UTF_8)), false))
        }
        dc.registerObserver(object : DataChannel.Observer {
            override fun onStateChange() {
                Log.d(TAG, "DataChannel state: ${dc.state()}")
                when (dc.state()) {
                    DataChannel.State.OPEN    -> scheduleSync()
                    DataChannel.State.CLOSED,
                    DataChannel.State.CLOSING -> {
                        sendFn = null
                        onClosed?.invoke()
                    }
                    else                      -> {}
                }
            }

            override fun onMessage(buffer: DataChannel.Buffer) {
                // SDK delivers buffer already flipped (position=0, limit=dataLength)
                val bytes = ByteArray(buffer.data.remaining())
                buffer.data.get(bytes)
                handleMessage(String(bytes, Charsets.UTF_8))
            }

            override fun onBufferedAmountChange(previousAmount: Long) {}
        })

        // Mirror datachannel.js lines 101-105:
        // onDataChannel fires on the viewer when the channel is already OPEN (streamer created
        // it before createOffer). In that case onStateChange(OPEN) was delivered before we
        // registered the observer, so scheduleSync() would never be called — kick it manually.
        if (dc.state() == DataChannel.State.OPEN) {
            Log.i(TAG, "DataChannel already OPEN at bind() — scheduling sync immediately")
            scheduleSync()
        }
    }

    /** Handle an incoming DataChannel message (JSON string). */
    fun handleMessage(raw: String) {
        try {
            val msg = JSONObject(raw)
            when (msg.getString("type")) {
                "pong" -> handlePong(msg)
                "ts"   -> handleTimestamp(msg)
                else   -> Log.d(TAG, "Unknown DC message type: ${msg.optString("type")}")
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error handling DC message: $raw", e)
        }
    }

    private fun handlePong(msg: JSONObject) {
        val id = msg.getInt("id")
        val t0 = pendingPings.remove(id) ?: return
        val t1 = msg.getLong("t1")
        val t2 = System.currentTimeMillis()
        val rtt = t2 - t0
        val offset = t1 - t0 - rtt / 2
        Log.d(TAG, "pong id=$id rtt=${rtt}ms offset=${offset}ms")
        collectedOffsets.add(offset)
    }

    private fun handleTimestamp(msg: JSONObject) {
        if (!clockSynced) {
            Log.i(TAG, "ts received but clockSynced=false — skipping (sync pending)")
            return
        }
        val capture = msg.getLong("capture")
        val encode  = msg.getLong("encode")
        val e2e = System.currentTimeMillis() - capture + clockOffset
        Log.i(TAG, "ts dispatched: e2e=${e2e}ms offset=${clockOffset}ms cbNull=${onTimestamp == null}")
        onTimestamp?.invoke(capture, encode)
    }

    /**
     * NTP-style clock synchronisation — mirrors datachannel.js syncClocks().
     *
     * Delayed 3 s after DataChannel open to avoid disturbing the jitter buffer
     * during the initial burst of WebRTC STUN/DTLS/RTCP packets.
     */
    fun scheduleSync() {
        scope.launch {
            delay(SYNC_STARTUP_DELAY_MS)
            syncClocks()
        }
    }

    private suspend fun syncClocks() {
        collectedOffsets.clear()
        pendingPings.clear()

        repeat(SYNC_ROUNDS) { i ->
            val t0 = System.currentTimeMillis()
            pendingPings[i] = t0
            sendFn?.invoke(JSONObject().apply {
                put("type", "ping")
                put("id", i)
                put("t0", t0)
            }.toString()) ?: run {
                Log.w(TAG, "sendFn not set — clock sync skipped")
                return
            }

            delay(PING_INTERVAL_MS * 4)   // wait for pong
        }

        if (collectedOffsets.isEmpty()) {
            Log.w(TAG, "No pongs received — clock sync failed")
            return
        }

        clockOffset = median(collectedOffsets)
        clockSynced = true   // @Volatile write — immediately visible to all threads
        Log.i(TAG, "Clock sync complete: offset=${clockOffset}ms (${collectedOffsets.size} samples)")
        onClockSynced?.invoke(clockOffset)
    }

    /**
     * Compute E2E latency for a captured frame.
     * @param captureMs  "capture" field from the "ts" message (streamer's Date.now())
     * @return latency in milliseconds
     */
    fun computeE2E(captureMs: Long): Long {
        return System.currentTimeMillis() - captureMs + clockOffset
    }

    fun getOffset(): Long = clockOffset
    fun isSynced(): Boolean = clockSynced

    private fun median(values: List<Long>): Long {
        val sorted = values.sorted()
        val mid = sorted.size / 2
        return if (sorted.size % 2 == 0) (sorted[mid - 1] + sorted[mid]) / 2 else sorted[mid]
    }
}
