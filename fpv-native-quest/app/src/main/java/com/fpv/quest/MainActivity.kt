package com.fpv.quest

import android.app.Activity
import android.os.Bundle
import android.util.Log

/**
 * Entry point for the FPV Quest native app.
 *
 * TASK-002: stub — compiles and launches on Quest 2, black screen.
 * TODO TASK-003: initialize SignalingClient + WebRTCEngine, start video stream.
 * TODO TASK-004: initialize OpenXR session and xr_renderer for stereo display.
 * TODO TASK-005: initialize FPVDataChannel for clock sync and E2E latency.
 */
class MainActivity : Activity() {

    companion object {
        private const val TAG = "FPVQuest"

        // Load the native library built from app/src/main/cpp/
        init {
            System.loadLibrary("fpv-native")
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Log.i(TAG, "FPV Quest native app starting (TASK-002 stub)")

        // TODO TASK-003: val engine = WebRTCEngine()
        //                engine.init(this)
        //                val signalingUrl = "ws://192.168.x.x:8080"
        //                val signaling = SignalingClient(...)
        //                signaling.connect(signalingUrl)

        // TODO TASK-004: initialize xr_renderer via JNI (nativeInitXR())
        // TODO TASK-005: engine.dataChannel.onClockSynced = { ... }
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
        // TODO TASK-003: signaling.close(); engine.close()
    }
}
