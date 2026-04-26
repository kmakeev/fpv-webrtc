#pragma once
// PROMPT-04: Camera OV5647 + hardware H.264 encoder (VEU)

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Колбэк при готовности каждого H.264 NAL-unit
// data      — буфер NAL-unit (valid только в scope колбэка)
// len       — размер в байтах
// pts_ms    — timestamp захвата (esp_timer_get_time() / 1000)
// encode_ms — время аппаратного H.264 кодирования в мс
typedef void (*camera_frame_cb_t)(const uint8_t *data, size_t len,
                                  uint64_t pts_ms, uint32_t encode_ms);

esp_err_t camera_init(camera_frame_cb_t frame_cb);
void      camera_deinit(void);

// Request that the next encoded frame be an IDR (keyframe).
// Thread-safe; may be called from any task (e.g. on WebRTC connect or PLI).
void      camera_request_idr(void);

// Dynamic bitrate control — used by ABR in webrtc_streamer.c.
// camera_set_bitrate(): updates H.264 rate-control without encoder restart.
// camera_get_bitrate(): returns current bitrate (bps); 0 if encoder not ready.
void      camera_set_bitrate(uint32_t bitrate_bps);
uint32_t  camera_get_bitrate(void);

// Request a resolution switch via DataChannel {type:'resolution', w, h}.
// Non-blocking: stores pending resolution; camera_task (core 1) applies it
// after the current encode completes, then fully restarts ISP + encoder.
void      camera_set_resolution(uint16_t w, uint16_t h);
