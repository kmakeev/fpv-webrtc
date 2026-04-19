#pragma once
// TODO: implemented in PROMPT-04

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
