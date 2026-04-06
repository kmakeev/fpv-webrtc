package com.fpv.quest

import org.webrtc.VideoFrame
import org.webrtc.VideoSink

/**
 * Zero-copy VideoSink for TASK-004.
 *
 * When HardwareVideoDecoderFactory is initialised with an EGL context,
 * hardware-decoded H.264 frames arrive here as VideoFrame.TextureBuffer
 * (type OES) — the frame stays in GPU memory, no CPU copy involved.
 *
 * For each such frame we fire [onOesFrame] with:
 *   - textureId      — GL_TEXTURE_EXTERNAL_OES name (managed by WebRTC's
 *                      internal SurfaceTextureHelper)
 *   - transformMatrix — float[9] from android.graphics.Matrix.getValues(),
 *                      row-major 3×3 affine transform; must be applied to
 *                      texture coordinates in the fragment shader (TASK-005).
 *
 * If a frame arrives as I420Buffer (software decoder fallback) the callback
 * is skipped — SurfaceViewRenderer will still display it normally.
 *
 * TASK-005 will consume the values stored by the callback to render each eye.
 */
class EglVideoSink(
    private val onOesFrame: (textureId: Int, transformMatrix: FloatArray) -> Unit
) : VideoSink {

    /** Total OES frames forwarded to native code (for diagnostics). */
    @Volatile var frameCount: Long = 0L
        private set

    // Reuse the float[9] scratch buffer — onFrame is called on a single decoder thread.
    private val matValues = FloatArray(9)

    override fun onFrame(frame: VideoFrame) {
        val buf = frame.buffer
        if (buf is VideoFrame.TextureBuffer &&
            buf.type == VideoFrame.TextureBuffer.Type.OES
        ) {
            // getTransformMatrix() returns android.graphics.Matrix (3×3 row-major affine).
            // getValues() fills a float[9] with the 9 matrix elements.
            buf.transformMatrix.getValues(matValues)
            onOesFrame(buf.textureId, matValues)
            frameCount++
        }
        // VideoSink does not own the frame — no retain/release needed.
    }
}
