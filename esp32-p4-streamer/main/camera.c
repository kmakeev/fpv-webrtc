// PROMPT-04: Camera OV5647 MIPI CSI + hardware H.264 encoder (VEU)
// Native IDF stack: esp_driver_cam + esp_driver_isp + espressif/esp_cam_sensor
//
// Pipeline:
//   OV5647 → MIPI CSI (RAW8) → ESP ISP → O_UYY_E_VYY PSRAM buffer
//   → H.264 HW VEU encoder → frame_cb
//
// ISP outputs O_UYY_E_VYY directly — confirmed by hexdump showing
// C Y Y | C Y Y pattern with stride = W*3/2. No CPU format conversion needed.
//
// 6-buffer ring: ensures DMA doesn't overwrite the buffer the encoder reads.
// Must satisfy: NUM_CAP_BUFS × frame_interval_ms > encode_time_ms.

#include "camera.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "driver/i2c_master.h"
#include "driver/isp.h"
#include "driver/isp_awb.h"
#include "driver/isp_ae.h"
#include "driver/isp_sharpen.h"
#include "driver/isp_color.h"
#include "driver/isp_gamma.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_ctlr.h"
#include "esp_sccb_intf.h"
#include "esp_sccb_i2c.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_h264_enc_single.h"
#include "esp_h264_enc_param.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <inttypes.h>
#include <string.h>

static const char *TAG = "camera";

// ──────────────────────────────────────────────
// Board-specific constants (ESP32-P4-Function-EV-Board)
// ──────────────────────────────────────────────

// OV5647 SCCB (I2C) pins — ESP32-P4-Function-EV-Board v1.4
// Camera SCCB is on LP I2C0: SDA=GPIO7, SCL=GPIO8
// Reference: esp-idf/examples/peripherals/camera/mipi_isp_dsi/main/example_config.h
// (GPIO31/34 seen in that example's log are for the LCD panel I2C, not camera)
#define CAM_SCCB_SDA_IO       7
#define CAM_SCCB_SCL_IO       8
#define CAM_SCCB_FREQ_HZ      100000

// MIPI PHY LDO (powers the MIPI CSI receiver on ESP32-P4)
#define CAM_LDO_CHAN_ID       3
#define CAM_LDO_VOLTAGE_MV    2500

// ──────────────────────────────────────────────
// Buffer sizes
// ──────────────────────────────────────────────

// Number of capture (DMA) buffers.
// Must satisfy: NUM_CAP_BUFS × frame_interval_ms > encode_time_ms
#define NUM_CAP_BUFS     6

// H.264 GOP (keyframe period): IDR every ~0.5s at 30fps.
// Shorter than enc_fps (1s) — faster recovery after packet loss.
#define ENC_GOP          15

// ──────────────────────────────────────────────
// Module state
// ──────────────────────────────────────────────

static camera_frame_cb_t      g_frame_cb  = NULL;
static esp_cam_ctlr_handle_t  g_cam_hdl   = NULL;
static esp_h264_enc_handle_t  g_encoder   = NULL;

// Actual sensor resolution — set at runtime from the selected sensor format.
static uint16_t  g_cam_width  = 0;
static uint16_t  g_cam_height = 0;
static size_t    g_cap_buf_len = 0;  // width * height * 3/2 (YUV420 / O_UYY_E_VYY)

// Current H.264 bitrate (bps) — updated by camera_init and camera_set_bitrate.
// Read by webrtc_streamer ABR from peer_loop task (core 0); written by camera_task
// (core 1) only during init/reconfigure. volatile suffices: atomic 32-bit RMW.
static volatile uint32_t g_cur_bitrate = 0;

// Sensor + ISP handles — promoted to globals so _camera_reconfigure() can
// tear down and recreate the pipeline without rerunning sensor detection.
static esp_cam_sensor_device_t *g_sensor   = NULL;
static isp_proc_handle_t        g_isp_hdl  = NULL;
static isp_awb_ctlr_t           g_awb_ctlr = NULL;
static isp_ae_ctlr_t            g_ae_ctlr  = NULL;

// Resolution switch: set by camera_set_resolution() (any task), consumed by
// camera_task (core 1) after the current encode completes.
static volatile bool     g_res_pending = false;
static volatile uint16_t g_pending_w   = 0;
static volatile uint16_t g_pending_h   = 0;

// Set by camera_request_idr(); camera_task forces GOP=1 on next encode,
// then restores GOP=g_enc_fps after the IDR is produced.
static volatile bool          g_force_idr = false;
static uint8_t                g_enc_fps   = 30;  // updated from sensor fps at init

// Capture buffers (PSRAM, manually 64-byte aligned).
// heap_caps_aligned_alloc(64, …, MALLOC_CAP_SPIRAM) on ESP32-P4 returns addresses
// that are only 32-byte aligned due to an allocator quirk (the 64-byte request is
// silently ignored).  The H264 encoder calls esp_cache_msync internally with the
// input address; if the address is not cache-line (64-byte) aligned it returns an
// error and enc_process fails for all frames after the first.
// Fix: allocate g_cap_buf_len + 63 bytes raw and manually round up to 64-byte boundary.
static uint8_t  *g_cap_raw[NUM_CAP_BUFS];  // raw pointers (for free); never used for DMA
static uint8_t  *g_cap_buf[NUM_CAP_BUFS];  // 64-byte-aligned pointers (used for DMA)
// Ring index: index of the buffer to use for the NEXT DMA transfer
static volatile int g_dma_next = 0;

// Queue: ISR → camera_task. Item = pointer to the just-captured PSRAM buffer.
// Depth 1 + overwrite: always has the latest frame; drops old if task is slow.
static QueueHandle_t g_frame_q = NULL;

// H.264 bitstream output buffer (manually 64-byte aligned, same reason)
static uint8_t  *g_h264_raw = NULL;
static uint8_t  *g_h264_buf = NULL;

// ──────────────────────────────────────────────
// CSI event callbacks  (must be IRAM-safe)
// ──────────────────────────────────────────────

// Called by DMA ISR to request the next capture buffer.
// Provides the "other" buffer in the ring and advances the ring index.
static IRAM_ATTR bool s_cam_get_new_trans(esp_cam_ctlr_handle_t handle,
                                           esp_cam_ctlr_trans_t *trans,
                                           void *user_data)
{
    int idx = g_dma_next;
    g_dma_next = (idx + 1) % NUM_CAP_BUFS;
    trans->buffer = g_cap_buf[idx];
    trans->buflen = g_cap_buf_len;
    return false;
}

// Called by DMA ISR when a frame capture is complete.
// Sends the buffer pointer to camera_task via an overwrite queue.
static IRAM_ATTR bool s_cam_trans_finished(esp_cam_ctlr_handle_t handle,
                                            esp_cam_ctlr_trans_t *trans,
                                            void *user_data)
{
    BaseType_t woken = pdFALSE;
    const uint8_t *buf = (const uint8_t *)trans->buffer;
    xQueueOverwriteFromISR(g_frame_q, &buf, &woken);
    return woken == pdTRUE;
}

// AWB statistics done callback (ISR context) — required for continuous mode.
// The ISP hardware applies AWB corrections internally; we just need to register
// the callback so start_continuous_statistics doesn't reject with "not registered".
static IRAM_ATTR bool s_awb_stats_done(isp_awb_ctlr_t ctlr,
                                        const esp_isp_awb_evt_data_t *edata,
                                        void *user_data)
{
    (void)ctlr; (void)edata; (void)user_data;
    return false;  // no high-priority task woken
}

// ──────────────────────────────────────────────
// Forward declaration — defined below camera_task
// ──────────────────────────────────────────────
static void _camera_reconfigure(uint16_t new_w, uint16_t new_h);

// ──────────────────────────────────────────────
// Camera + encode task  (pinned to core 1)
// ──────────────────────────────────────────────

void camera_request_idr(void)
{
    g_force_idr = true;
}

// ── Bitrate control (ABR) ─────────────────────────────────────────────────────

uint32_t camera_get_bitrate(void)
{
    return g_cur_bitrate;
}

void camera_set_bitrate(uint32_t bitrate_bps)
{
    if (!g_encoder || bitrate_bps == 0) return;
    esp_h264_enc_param_hw_handle_t hw_ph = NULL;
    if (esp_h264_enc_hw_get_param_hd(g_encoder, &hw_ph) != ESP_H264_ERR_OK) return;
    if (esp_h264_enc_set_bitrate((esp_h264_enc_param_handle_t)hw_ph, bitrate_bps)
            == ESP_H264_ERR_OK) {
        g_cur_bitrate = bitrate_bps;
        ESP_LOGI(TAG, "ABR: bitrate → %" PRIu32 " bps", bitrate_bps);
    } else {
        ESP_LOGW(TAG, "ABR: esp_h264_enc_set_bitrate failed");
    }
}

// ── Resolution switch (async, applied in camera_task) ────────────────────────

void camera_set_resolution(uint16_t w, uint16_t h)
{
    if (w == 0 || h == 0) return;
    g_pending_w   = w;
    g_pending_h   = h;
    g_res_pending = true;
    ESP_LOGI(TAG, "Resolution switch requested: %ux%u (pending)", w, h);
}

// Helper: get the encoder param handle (used to change GOP).
static esp_h264_enc_param_handle_t s_get_param_hd(void)
{
    esp_h264_enc_param_hw_handle_t hw_ph = NULL;
    if (esp_h264_enc_hw_get_param_hd(g_encoder, &hw_ph) != ESP_H264_ERR_OK) return NULL;
    return (esp_h264_enc_param_handle_t)hw_ph;
}

static void camera_task(void *arg)
{
    bool first_frame = true;
    const uint8_t *frame_buf = NULL;
    uint32_t task_frame_count = 0;

    // Task 1: FPS measurement
    static int64_t  s_fps_ts_us    = 0;
    static uint32_t s_fps_frame_ref = 0;
    // Task 6: encode-time statistics
    static uint32_t s_enc_sum_ms = 0;
    static uint32_t s_enc_count  = 0;

    while (1) {
        if (xQueueReceive(g_frame_q, &frame_buf, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        task_frame_count++;
        // Heartbeat: first frame + every 30 frames — with real FPS
        if (task_frame_count == 1 || task_frame_count % 30 == 0) {
            int64_t now_us = esp_timer_get_time();
            if (s_fps_ts_us > 0 && task_frame_count > s_fps_frame_ref) {
                float elapsed_s = (float)(now_us - s_fps_ts_us) / 1e6f;
                float fps = (float)(task_frame_count - s_fps_frame_ref) / elapsed_s;
                ESP_LOGI(TAG, "camera_task: frame #%"PRIu32" — %.1f fps (task-side)",
                         task_frame_count, fps);
            } else {
                ESP_LOGI(TAG, "camera_task: frame #%"PRIu32, task_frame_count);
            }
            s_fps_ts_us     = now_us;
            s_fps_frame_ref = task_frame_count;
        }

        // Force IDR: set GOP=1 so the encoder sees a GOP change and emits IDR.
        // GOP is restored to g_enc_fps after the IDR is confirmed below.
        if (g_force_idr) {
            g_force_idr = false;
            esp_h264_enc_param_handle_t ph = s_get_param_hd();
            if (ph) {
                esp_h264_enc_set_gop(ph, 1);
                ESP_LOGI(TAG, "IDR force at frame #%"PRIu32, task_frame_count);
            }
        }

        uint64_t capture_ts = (uint64_t)(esp_timer_get_time() / 1000);
        uint64_t t0         = esp_timer_get_time();

        // ISP outputs O_UYY_E_VYY directly — pass frame_buf straight to encoder.
        // No format conversion needed (confirmed by hexdump: C Y Y pattern,
        // stride = W*3/2).
        esp_h264_enc_in_frame_t in_frame = {
            .raw_data = { .buffer = (uint8_t *)frame_buf, .len = (uint32_t)g_cap_buf_len },
        };
        esp_h264_enc_out_frame_t out_frame = {
            .raw_data = { .buffer = g_h264_buf, .len = (uint32_t)g_cap_buf_len },
        };

        esp_h264_err_t ret = esp_h264_enc_process(g_encoder, &in_frame, &out_frame);
        if (ret == ESP_H264_ERR_OK && out_frame.length > 0) {
            uint32_t encode_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
            if (first_frame) {
                ESP_LOGI(TAG, "First H.264 frame: %"PRIu32" bytes, encode=%"PRIu32" ms",
                         out_frame.length, encode_ms);
                first_frame = false;
            }
            s_enc_sum_ms += encode_ms;
            s_enc_count++;
            if (s_enc_count % 150 == 0) {
                uint32_t avg_ms = s_enc_sum_ms / s_enc_count;
                ESP_LOGI(TAG, "H.264 encode avg=%"PRIu32"ms over %"PRIu32" frames",
                         avg_ms, s_enc_count);
                s_enc_sum_ms = 0;
                s_enc_count  = 0;
            }

            // After a forced IDR (or any IDR while GOP != ENC_GOP), restore GOP.
            if (out_frame.frame_type == ESP_H264_FRAME_TYPE_IDR) {
                esp_h264_enc_param_handle_t ph = s_get_param_hd();
                if (ph) {
                    uint8_t cur_gop = 0;
                    esp_h264_enc_get_gop(ph, &cur_gop);
                    if (cur_gop != ENC_GOP) {
                        esp_h264_enc_set_gop(ph, ENC_GOP);
                        ESP_LOGI(TAG, "IDR sent — GOP restored to %d", ENC_GOP);
                    }
                }
            }

            if (g_frame_cb) {
                g_frame_cb(out_frame.raw_data.buffer, (size_t)out_frame.length,
                           capture_ts, encode_ms);
            }

            // Resolution switch: applied after frame delivery so the callback
            // always gets a complete frame before the pipeline restarts.
            if (g_res_pending) {
                g_res_pending = false;
                _camera_reconfigure(g_pending_w, g_pending_h);
            }
        } else if (ret != ESP_H264_ERR_OK) {
            ESP_LOGW(TAG, "H.264 enc_process error: %d", (int)ret);
        } else {
            // ret == OK but length == 0 — encoder skipped this frame
            static uint32_t s_zero_len_count = 0;
            s_zero_len_count++;
            if (s_zero_len_count % 30 == 1) {
                ESP_LOGW(TAG, "enc_process length=0 (count=%"PRIu32")", s_zero_len_count);
            }
        }
    }
}

// ──────────────────────────────────────────────
// _camera_reconfigure — tear down ISP+encoder, re-init with new resolution
// Runs on camera_task (core 1). g_sensor must be valid.
// ──────────────────────────────────────────────

static void _camera_reconfigure(uint16_t new_w, uint16_t new_h)
{
    ESP_LOGI(TAG, "_camera_reconfigure: %ux%u → %ux%u",
             g_cam_width, g_cam_height, new_w, new_h);

    // ── 1. Close H.264 encoder ──
    if (g_encoder) {
        esp_h264_enc_close(g_encoder);
        esp_h264_enc_del(g_encoder);
        g_encoder = NULL;
    }
    heap_caps_free(g_h264_raw);
    g_h264_raw = NULL;
    g_h264_buf = NULL;

    // ── 2. Tear down ISP controllers (AWB/AE/sharpen/color/gamma) ──
    if (g_awb_ctlr) {
        esp_isp_awb_controller_stop_continuous_statistics(g_awb_ctlr);
        esp_isp_awb_controller_disable(g_awb_ctlr);
        esp_isp_del_awb_controller(g_awb_ctlr);
        g_awb_ctlr = NULL;
    }
    if (g_ae_ctlr) {
        esp_isp_ae_controller_stop_continuous_statistics(g_ae_ctlr);
        esp_isp_ae_controller_disable(g_ae_ctlr);
        esp_isp_del_ae_controller(g_ae_ctlr);
        g_ae_ctlr = NULL;
    }

    // ── 3. Disable + delete ISP ──
    if (g_isp_hdl) {
        esp_isp_sharpen_disable(g_isp_hdl);
        esp_isp_color_disable(g_isp_hdl);
        esp_isp_gamma_disable(g_isp_hdl);
        esp_isp_disable(g_isp_hdl);
        esp_isp_del_processor(g_isp_hdl);
        g_isp_hdl = NULL;
    }

    // ── 4. Stop + disable + delete CSI ──
    if (g_cam_hdl) {
        esp_cam_ctlr_stop(g_cam_hdl);
        esp_cam_ctlr_disable(g_cam_hdl);
        esp_cam_ctlr_del(g_cam_hdl);
        g_cam_hdl = NULL;
    }

    // ── 5. Free old capture buffers ──
    for (int i = 0; i < NUM_CAP_BUFS; i++) {
        heap_caps_free(g_cap_raw[i]);
        g_cap_raw[i] = NULL;
        g_cap_buf[i] = NULL;
    }

    // ── 6. Find sensor format for new resolution ──
    if (!g_sensor) {
        ESP_LOGE(TAG, "_camera_reconfigure: g_sensor is NULL — cannot reconfigure");
        return;
    }
    esp_cam_sensor_format_array_t fmt_arr = {0};
    esp_cam_sensor_query_format(g_sensor, &fmt_arr);

    const esp_cam_sensor_format_t *selected = NULL;
    int best_score = -1;
    for (int i = 0; i < (int)fmt_arr.count; i++) {
        const esp_cam_sensor_format_t *f = &fmt_arr.format_array[i];
        if (f->width < f->height) continue;
        // Exact match preferred; fall back to score-based selection
        int score = 0;
        if (f->width == new_w && f->height == new_h) score += 1000;
        if (f->format != ESP_CAM_SENSOR_PIXFORMAT_RAW10) score += 100;
        if (f->width <= 800 && f->height <= 640)  score += 50;
        else if (f->width <= 800)                  score += 30;
        else if (f->width <= 1280)                 score += 10;
        if (f->fps <= 30) score += 5;
        if (score > best_score) { best_score = score; selected = f; }
    }
    if (!selected && fmt_arr.count > 0) selected = &fmt_arr.format_array[0];
    if (!selected) {
        ESP_LOGE(TAG, "_camera_reconfigure: no sensor format found");
        return;
    }
    ESP_LOGI(TAG, "Reconfigure: using format %s (%ux%u @ %u fps)",
             selected->name, selected->width, selected->height, selected->fps);

    // ── 7. Set new sensor format + update global dims ──
    if (esp_cam_sensor_set_format(g_sensor, selected) != ESP_OK) {
        ESP_LOGE(TAG, "_camera_reconfigure: sensor set_format failed");
        return;
    }
    g_cam_width   = selected->width;
    g_cam_height  = selected->height;
    g_cap_buf_len = (size_t)g_cam_width * g_cam_height * 3 / 2;

    cam_ctlr_color_t csi_input_color;
    isp_color_t      isp_input_color;
    if (selected->format == ESP_CAM_SENSOR_PIXFORMAT_RAW10) {
        csi_input_color = CAM_CTLR_COLOR_RAW10;
        isp_input_color = ISP_COLOR_RAW10;
    } else {
        csi_input_color = CAM_CTLR_COLOR_RAW8;
        isp_input_color = ISP_COLOR_RAW8;
    }
    uint32_t mipi_mbps  = selected->mipi_info.mipi_clk / 1000000UL;
    int      mipi_lanes = (int)selected->mipi_info.lane_num;

    // ── 8. Allocate new capture buffers ──
    g_dma_next = 0;
    for (int i = 0; i < NUM_CAP_BUFS; i++) {
        g_cap_raw[i] = heap_caps_malloc(g_cap_buf_len + 63,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!g_cap_raw[i]) {
            ESP_LOGE(TAG, "_camera_reconfigure: cap_buf[%d] alloc failed", i);
            return;
        }
        g_cap_buf[i] = (uint8_t *)(((uintptr_t)g_cap_raw[i] + 63) & ~(uintptr_t)63);
    }

    // ── 9. Recreate CSI ──
    esp_cam_ctlr_csi_config_t csi_cfg = {
        .ctlr_id               = 0,
        .h_res                 = g_cam_width,
        .v_res                 = g_cam_height,
        .data_lane_num         = mipi_lanes,
        .lane_bit_rate_mbps    = mipi_mbps,
        .input_data_color_type = csi_input_color,
        .output_data_color_type= CAM_CTLR_COLOR_YUV420,
        .queue_items           = 1,
        .byte_swap_en          = false,
        .bk_buffer_dis         = true,
    };
    if (esp_cam_new_csi_ctlr(&csi_cfg, &g_cam_hdl) != ESP_OK) {
        ESP_LOGE(TAG, "_camera_reconfigure: CSI create failed"); return;
    }
    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans  = s_cam_get_new_trans,
        .on_trans_finished = s_cam_trans_finished,
    };
    if (esp_cam_ctlr_register_event_callbacks(g_cam_hdl, &cbs, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "_camera_reconfigure: CSI callback register failed"); return;
    }
    if (esp_cam_ctlr_enable(g_cam_hdl) != ESP_OK) {
        ESP_LOGE(TAG, "_camera_reconfigure: CSI enable failed"); return;
    }

    // ── 10. Recreate ISP ──
    esp_isp_processor_cfg_t isp_cfg = {
        .clk_hz                 = 80 * 1000 * 1000,
        .input_data_source      = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type  = isp_input_color,
        .output_data_color_type = ISP_COLOR_YUV420,
        .has_line_start_packet  = selected->mipi_info.line_sync_en,
        .has_line_end_packet    = false,
        .h_res                  = g_cam_width,
        .v_res                  = g_cam_height,
    };
    g_isp_hdl = NULL;
    if (esp_isp_new_processor(&isp_cfg, &g_isp_hdl) != ESP_OK) {
        ESP_LOGE(TAG, "_camera_reconfigure: ISP create failed"); return;
    }
    esp_isp_enable(g_isp_hdl);

    // ── 11. Recreate ISP modules ──
    esp_isp_awb_config_t awb_cfg = {
        .sample_point = ISP_AWB_SAMPLE_POINT_AFTER_CCM,
        .window = {
            .top_left  = { .x = g_cam_width / 5,     .y = g_cam_height / 5 },
            .btm_right = { .x = g_cam_width * 4 / 5, .y = g_cam_height * 4 / 5 },
        },
        .white_patch = {
            .luminance        = { .min = 0, .max = 220 * 3 },
            .red_green_ratio  = { .min = 0.0f, .max = 3.999f },
            .blue_green_ratio = { .min = 0.0f, .max = 3.999f },
        },
    };
    g_awb_ctlr = NULL;
    if (esp_isp_new_awb_controller(g_isp_hdl, &awb_cfg, &g_awb_ctlr) == ESP_OK) {
        esp_isp_awb_cbs_t awb_cbs = { .on_statistics_done = s_awb_stats_done };
        esp_isp_awb_register_event_callbacks(g_awb_ctlr, &awb_cbs, NULL);
        esp_isp_awb_controller_enable(g_awb_ctlr);
        esp_isp_awb_controller_start_continuous_statistics(g_awb_ctlr);
    }
    esp_isp_ae_config_t ae_cfg = { .sample_point = ISP_AE_SAMPLE_POINT_AFTER_DEMOSAIC };
    g_ae_ctlr = NULL;
    if (esp_isp_new_ae_controller(g_isp_hdl, &ae_cfg, &g_ae_ctlr) == ESP_OK) {
        esp_isp_ae_controller_enable(g_ae_ctlr);
        esp_isp_ae_controller_start_continuous_statistics(g_ae_ctlr);
    }
    esp_isp_sharpen_config_t sharpen_cfg = {
        .h_freq_coeff = { .val = (2 << ISP_SHARPEN_H_FREQ_COEF_DEC_BITS) },
        .m_freq_coeff = { .val = (1 << ISP_SHARPEN_M_FREQ_COEF_DEC_BITS) | (1 << (ISP_SHARPEN_M_FREQ_COEF_DEC_BITS - 1)) },
        .h_thresh = 30, .l_thresh = 5,
        .padding_mode = ISP_SHARPEN_EDGE_PADDING_MODE_SRND_DATA,
        .sharpen_template = { {1,2,1}, {2,4,2}, {1,2,1} },
    };
    if (esp_isp_sharpen_configure(g_isp_hdl, &sharpen_cfg) == ESP_OK)
        esp_isp_sharpen_enable(g_isp_hdl);
    esp_isp_color_config_t color_cfg = {
        .color_contrast   = { .val = (1 << 7) | 64 },
        .color_saturation = { .val = (1 << 7) | 127 },
        .color_hue        = 0,
        .color_brightness = -15,
    };
    if (esp_isp_color_configure(g_isp_hdl, &color_cfg) == ESP_OK)
        esp_isp_color_enable(g_isp_hdl);
    isp_gamma_curve_points_t gamma_pts = { .pt = {
        { .x= 16,.y= 28},{ .x= 32,.y= 48},{ .x= 48,.y= 64},{ .x= 64,.y= 79},
        { .x= 80,.y= 93},{ .x= 96,.y=107},{ .x=112,.y=119},{ .x=128,.y=132},
        { .x=144,.y=144},{ .x=160,.y=156},{ .x=176,.y=168},{ .x=192,.y=180},
        { .x=208,.y=192},{ .x=224,.y=204},{ .x=240,.y=216},{ .x=255,.y=255},
    }};
    esp_isp_gamma_configure(g_isp_hdl, COLOR_COMPONENT_R, &gamma_pts);
    esp_isp_gamma_configure(g_isp_hdl, COLOR_COMPONENT_G, &gamma_pts);
    esp_isp_gamma_configure(g_isp_hdl, COLOR_COMPONENT_B, &gamma_pts);
    esp_isp_gamma_enable(g_isp_hdl);

    // ── 12. Start CSI + stream-on sensor ──
    if (esp_cam_ctlr_start(g_cam_hdl) != ESP_OK) {
        ESP_LOGE(TAG, "_camera_reconfigure: CSI start failed"); return;
    }
    int stream_on = 1;
    if (esp_cam_sensor_ioctl(g_sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &stream_on) != ESP_OK) {
        ESP_LOGE(TAG, "_camera_reconfigure: sensor stream-on failed");
    }

    // ── 13. Allocate H.264 output buffer ──
    g_h264_raw = heap_caps_malloc(g_cap_buf_len + 63, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_h264_raw) {
        ESP_LOGE(TAG, "_camera_reconfigure: H264 buf alloc failed"); return;
    }
    g_h264_buf = (uint8_t *)(((uintptr_t)g_h264_raw + 63) & ~(uintptr_t)63);

    // ── 14. Recreate H.264 encoder with new resolution ──
    uint8_t enc_fps    = g_enc_fps;
    uint32_t pixels    = (uint32_t)g_cam_width * g_cam_height;
    uint32_t bitrate   = (uint32_t)(4000000ULL * pixels / (1280 * 720));
    if (bitrate < 1500000) bitrate = 1500000;
    if (bitrate > 8000000) bitrate = 8000000;
    esp_h264_enc_cfg_hw_t enc_cfg = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
        .gop      = ENC_GOP,
        .fps      = enc_fps,
        .res      = { .width = g_cam_width, .height = g_cam_height },
        .rc       = { .bitrate = bitrate, .qp_min = 20, .qp_max = 38 },
    };
    g_encoder = NULL;
    if (esp_h264_enc_hw_new(&enc_cfg, &g_encoder) != ESP_H264_ERR_OK ||
        esp_h264_enc_open(g_encoder) != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "_camera_reconfigure: H264 encoder init failed");
        g_encoder = NULL;
        return;
    }
    g_cur_bitrate = bitrate;

    ESP_LOGI(TAG, "Resolution reconfigured: %ux%u @ fps=%u bitrate=%" PRIu32,
             g_cam_width, g_cam_height, enc_fps, bitrate);
}

// ──────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────

esp_err_t camera_init(camera_frame_cb_t frame_cb)
{
    g_frame_cb = frame_cb;

    // ── 1. MIPI PHY LDO (channel 3, 2500 mV on ESP32-P4-Function-EV-Board) ──
    esp_ldo_channel_handle_t ldo_hdl = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = CAM_LDO_CHAN_ID,
        .voltage_mv = CAM_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo_hdl),
                        TAG, "MIPI PHY LDO init failed");

    // ── 2. I2C master bus for OV5647 SCCB ──
    i2c_master_bus_handle_t i2c_hdl = NULL;
    i2c_master_bus_config_t i2c_cfg = {
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .sda_io_num                   = CAM_SCCB_SDA_IO,
        .scl_io_num                   = CAM_SCCB_SCL_IO,
        .i2c_port                     = I2C_NUM_0,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &i2c_hdl),
                        TAG, "I2C master bus init failed");

    // ── Diagnostic: scan I2C bus to find actual sensor address ──
    {
        bool found_any = false;
        for (uint8_t addr = 0x01; addr < 0x7F; addr++) {
            if (i2c_master_probe(i2c_hdl, addr, 10) == ESP_OK) {
                ESP_LOGI(TAG, "I2C scan: device at 0x%02X", addr);
                found_any = true;
            }
        }
        if (!found_any) {
            ESP_LOGW(TAG, "I2C scan: no devices found on SDA=%d SCL=%d — check wiring",
                     CAM_SCCB_SDA_IO, CAM_SCCB_SCL_IO);
        }
    }

    // ── 4. Auto-detect camera sensor ──
    esp_cam_sensor_config_t sensor_cfg = {
        .reset_pin   = -1,
        .pwdn_pin    = -1,
        .xclk_pin    = -1,
    };
    g_sensor = NULL;

    for (esp_cam_sensor_detect_fn_t *p = &__esp_cam_sensor_detect_fn_array_start;
         p < &__esp_cam_sensor_detect_fn_array_end; ++p) {
        sccb_i2c_config_t sccb_cfg = {
            .scl_speed_hz    = CAM_SCCB_FREQ_HZ,
            .device_address  = p->sccb_addr,
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        };
        if (sccb_new_i2c_io(i2c_hdl, &sccb_cfg, &sensor_cfg.sccb_handle) != ESP_OK) {
            continue;
        }
        sensor_cfg.sensor_port = p->port;
        g_sensor = (*(p->detect))(&sensor_cfg);
        if (g_sensor) {
            ESP_LOGI(TAG, "Camera sensor detected");
            break;
        }
        esp_sccb_del_i2c_io(sensor_cfg.sccb_handle);
    }
    if (!g_sensor) {
        ESP_LOGE(TAG, "No camera sensor detected on I2C bus (SDA=%d SCL=%d)",
                 CAM_SCCB_SDA_IO, CAM_SCCB_SCL_IO);
        return ESP_ERR_NOT_FOUND;
    }

    // ── 5. Select sensor format ──
    // The selected format drives ALL downstream config (CSI, ISP, encoder).
    //
    // Format selection priority (landscape-only):
    //   1. 800×640 RAW8 — safe baseline: fast encode (~25ms), huge DMA margin,
    //      RAW8 ISP path is proven in esp-idf examples
    //   2. 800×800 RAW8 — acceptable square format
    //   3. any other landscape format — last resort
    //
    // RAW10 1280×960@45fps is avoided: encode ~98ms vs ring cycle 132ms leaves
    // only 34ms margin; IDR frames overshoot → DMA buffer race → noise.
    esp_cam_sensor_format_array_t fmt_arr = {0};
    esp_cam_sensor_query_format(g_sensor, &fmt_arr);

    const esp_cam_sensor_format_t *selected = NULL;
    int  best_score = -1;

    for (int i = 0; i < (int)fmt_arr.count; i++) {
        const esp_cam_sensor_format_t *f = &fmt_arr.format_array[i];
        ESP_LOGI(TAG, "Sensor fmt[%d]: %s (w=%u h=%u fps=%u)",
                 i, f->name, f->width, f->height, f->fps);

        if (f->width < f->height) continue;  // skip portrait modes

        int score = 0;
        // Strongly prefer RAW8 (proven ISP path, simpler processing)
        if (f->format != ESP_CAM_SENSOR_PIXFORMAT_RAW10) score += 100;
        // Prefer smaller resolutions (faster encode, bigger DMA margin)
        if (f->width <= 800 && f->height <= 640)  score += 50;
        else if (f->width <= 800)                  score += 30;
        else if (f->width <= 1280)                 score += 10;
        // Prefer lower fps (less encoder pressure)
        if (f->fps <= 30) score += 5;

        if (score > best_score) {
            best_score = score;
            selected   = f;
        }
    }

    if (!selected) {
        // Absolute fallback: pick first available
        for (int i = 0; i < (int)fmt_arr.count; i++) {
            if (fmt_arr.format_array[i].width >= fmt_arr.format_array[i].height) {
                selected = &fmt_arr.format_array[i];
                break;
            }
        }
    }
    if (!selected || fmt_arr.count == 0) {
        ESP_LOGE(TAG, "Sensor reports no usable formats");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Using sensor format: %s (%ux%u @ %u fps)",
             selected->name, selected->width, selected->height, selected->fps);

    // Derive runtime resolution and buffer sizes from the selected format.
    g_cam_width   = selected->width;
    g_cam_height  = selected->height;
    g_cap_buf_len = (size_t)g_cam_width * g_cam_height * 3 / 2;

    // Determine RAW input depth (RAW8 vs RAW10) for CSI + ISP.
    cam_ctlr_color_t csi_input_color;
    isp_color_t      isp_input_color;
    if (selected->format == ESP_CAM_SENSOR_PIXFORMAT_RAW10) {
        csi_input_color = CAM_CTLR_COLOR_RAW10;
        isp_input_color = ISP_COLOR_RAW10;
        ESP_LOGI(TAG, "Sensor RAW10 mode");
    } else {
        csi_input_color = CAM_CTLR_COLOR_RAW8;
        isp_input_color = ISP_COLOR_RAW8;
        ESP_LOGI(TAG, "Sensor RAW8 mode");
    }

    // MIPI params directly from sensor descriptor.
    // mipi_clk is in Hz; CSI driver wants Mbps.
    uint32_t mipi_mbps  = selected->mipi_info.mipi_clk / 1000000UL;
    int      mipi_lanes = (int)selected->mipi_info.lane_num;
    ESP_LOGI(TAG, "MIPI: %d lanes @ %"PRIu32" Mbps", mipi_lanes, mipi_mbps);

    ESP_RETURN_ON_ERROR(esp_cam_sensor_set_format(g_sensor, selected),
                        TAG, "Sensor set_format failed");

    // NOTE: sensor stream-on is deferred to step 10, AFTER CSI+ISP are ready.
    // Starting the sensor here would push MIPI data before any receiver exists,
    // causing the CSI to miss the first frame-start and never complete a DMA
    // transfer (symptom: camera_task receives zero frames).

    // ── 6. Allocate capture buffers (NUM_CAP_BUFS×, PSRAM, manually 64-byte aligned) ──
    for (int i = 0; i < NUM_CAP_BUFS; i++) {
        g_cap_raw[i] = heap_caps_malloc(g_cap_buf_len + 63,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!g_cap_raw[i]) {
            ESP_LOGE(TAG, "Failed to alloc capture buffer %d (%zu bytes)",
                     i, g_cap_buf_len + 63);
            return ESP_ERR_NO_MEM;
        }
        g_cap_buf[i] = (uint8_t *)(((uintptr_t)g_cap_raw[i] + 63) & ~(uintptr_t)63);
        ESP_LOGI(TAG, "cap_buf[%d]: raw=%p aligned=%p", i, g_cap_raw[i], g_cap_buf[i]);
    }

    // ── 7. Create frame-ready queue (depth 1, overwrite on full) ──
    g_frame_q = xQueueCreate(1, sizeof(uint8_t *));
    if (!g_frame_q) {
        return ESP_ERR_NO_MEM;
    }

    // ── 8. CSI controller + ISP ──
    // Init order matches esp-idf/examples/peripherals/camera/mipi_isp_dsi:
    //   CSI create → register callbacks → enable → ISP create → ISP enable → CSI start
    // ISP must be enabled before CSI starts receiving data.

    esp_cam_ctlr_csi_config_t csi_cfg = {
        .ctlr_id               = 0,
        .h_res                 = g_cam_width,
        .v_res                 = g_cam_height,
        .data_lane_num         = mipi_lanes,
        .lane_bit_rate_mbps    = mipi_mbps,
        .input_data_color_type = csi_input_color,
        .output_data_color_type= CAM_CTLR_COLOR_YUV420,
        .queue_items           = 1,
        .byte_swap_en          = false,
        .bk_buffer_dis         = true,
    };
    ESP_RETURN_ON_ERROR(esp_cam_new_csi_ctlr(&csi_cfg, &g_cam_hdl),
                        TAG, "CSI controller init failed");

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans  = s_cam_get_new_trans,
        .on_trans_finished = s_cam_trans_finished,
    };
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_register_event_callbacks(g_cam_hdl, &cbs, NULL),
                        TAG, "CSI callback register failed");

    ESP_RETURN_ON_ERROR(esp_cam_ctlr_enable(g_cam_hdl),
                        TAG, "CSI enable failed");

    // ISP: RAW→YUV420 (O_UYY_E_VYY), resolution from sensor format
    g_isp_hdl = NULL;
    esp_isp_processor_cfg_t isp_cfg = {
        .clk_hz                 = 80 * 1000 * 1000,
        .input_data_source      = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type  = isp_input_color,
        .output_data_color_type = ISP_COLOR_YUV420,
        .has_line_start_packet  = selected->mipi_info.line_sync_en,
        .has_line_end_packet    = false,
        .h_res                  = g_cam_width,
        .v_res                  = g_cam_height,
    };
    ESP_RETURN_ON_ERROR(esp_isp_new_processor(&isp_cfg, &g_isp_hdl),
                        TAG, "ISP init failed");
    ESP_RETURN_ON_ERROR(esp_isp_enable(g_isp_hdl),
                        TAG, "ISP enable failed");

    // ── AWB: auto white balance ──
    esp_isp_awb_config_t awb_cfg = {
        .sample_point = ISP_AWB_SAMPLE_POINT_AFTER_CCM,
        .window = {
            .top_left  = { .x = g_cam_width / 5,     .y = g_cam_height / 5 },
            .btm_right = { .x = g_cam_width * 4 / 5, .y = g_cam_height * 4 / 5 },
        },
        .white_patch = {
            .luminance        = { .min = 0, .max = 220 * 3 },
            .red_green_ratio  = { .min = 0.0f, .max = 3.999f },
            .blue_green_ratio = { .min = 0.0f, .max = 3.999f },
        },
    };
    g_awb_ctlr = NULL;
    esp_err_t awb_ret = esp_isp_new_awb_controller(g_isp_hdl, &awb_cfg, &g_awb_ctlr);
    if (awb_ret == ESP_OK) {
        // Register callback — required by start_continuous_statistics
        esp_isp_awb_cbs_t awb_cbs = { .on_statistics_done = s_awb_stats_done };
        esp_isp_awb_register_event_callbacks(g_awb_ctlr, &awb_cbs, NULL);
        esp_isp_awb_controller_enable(g_awb_ctlr);
        esp_isp_awb_controller_start_continuous_statistics(g_awb_ctlr);
        ESP_LOGI(TAG, "ISP AWB controller started");
    } else {
        ESP_LOGW(TAG, "ISP AWB init failed (%s)", esp_err_to_name(awb_ret));
    }

    // ── AE: auto exposure ──
    esp_isp_ae_config_t ae_cfg = {
        .sample_point = ISP_AE_SAMPLE_POINT_AFTER_DEMOSAIC,
    };
    g_ae_ctlr = NULL;
    esp_err_t ae_ret = esp_isp_new_ae_controller(g_isp_hdl, &ae_cfg, &g_ae_ctlr);
    if (ae_ret == ESP_OK) {
        esp_isp_ae_controller_enable(g_ae_ctlr);
        esp_isp_ae_controller_start_continuous_statistics(g_ae_ctlr);
        ESP_LOGI(TAG, "ISP AE controller started");
    } else {
        ESP_LOGW(TAG, "ISP AE init failed (%s)", esp_err_to_name(ae_ret));
    }

    // ── ISP Sharpen: edge enhancement ──
    // h_freq_coeff=2.0, m_freq_coeff=1.5 — noticeable sharpening for FPV.
    // Lower thresholds (h=30, l=5) to catch more edges.
    esp_isp_sharpen_config_t sharpen_cfg = {
        .h_freq_coeff = { .val = (2 << ISP_SHARPEN_H_FREQ_COEF_DEC_BITS) },                                   // 2.0
        .m_freq_coeff = { .val = (1 << ISP_SHARPEN_M_FREQ_COEF_DEC_BITS) | (1 << (ISP_SHARPEN_M_FREQ_COEF_DEC_BITS - 1)) },  // 1.5
        .h_thresh = 30,
        .l_thresh = 5,
        .padding_mode = ISP_SHARPEN_EDGE_PADDING_MODE_SRND_DATA,
        .sharpen_template = {
            {1, 2, 1},
            {2, 4, 2},
            {1, 2, 1},
        },
    };
    esp_err_t shp_ret = esp_isp_sharpen_configure(g_isp_hdl, &sharpen_cfg);
    if (shp_ret == ESP_OK) {
        esp_isp_sharpen_enable(g_isp_hdl);
        ESP_LOGI(TAG, "ISP Sharpen enabled (h_coeff=2.0, m_coeff=1.5)");
    } else {
        ESP_LOGW(TAG, "ISP Sharpen configure failed (%s)", esp_err_to_name(shp_ret));
    }

    // ── ISP Color: contrast + saturation boost ──
    // Default is 1.0 for both — grey washed-out image needs more contrast/saturation.
    // Fixed-point: integer(1 bit) + decimal(7 bits). 1.0 = 0x80, 1.3 = 0x80|38, 1.5 = 0x80|64
    esp_isp_color_config_t color_cfg = {
        .color_contrast   = { .val = (1 << 7) | 64 },  // 1.5 — strong contrast
        .color_saturation = { .val = (1 << 7) | 127 }, // ~2.0 — max vivid colors
        .color_hue        = 0,
        .color_brightness = -15,                        // compensate gamma lift
    };
    esp_err_t clr_ret = esp_isp_color_configure(g_isp_hdl, &color_cfg);
    if (clr_ret == ESP_OK) {
        esp_isp_color_enable(g_isp_hdl);
        ESP_LOGI(TAG, "ISP Color enabled (contrast=1.5, saturation=2.0, brightness=-15)");
    } else {
        ESP_LOGW(TAG, "ISP Color configure failed (%s)", esp_err_to_name(clr_ret));
    }

    // ── ISP Gamma: subtle lift, keep image darker ──
    // gamma ≈ 1/1.3: minimal shadow lift, preserves natural contrast.
    // y = 255 * pow(x/255, 1/1.3)
    isp_gamma_curve_points_t gamma_pts = { .pt = {
        { .x =  16, .y =  28 },
        { .x =  32, .y =  48 },
        { .x =  48, .y =  64 },
        { .x =  64, .y =  79 },
        { .x =  80, .y =  93 },
        { .x =  96, .y = 107 },
        { .x = 112, .y = 119 },
        { .x = 128, .y = 132 },
        { .x = 144, .y = 144 },
        { .x = 160, .y = 156 },
        { .x = 176, .y = 168 },
        { .x = 192, .y = 180 },
        { .x = 208, .y = 192 },
        { .x = 224, .y = 204 },
        { .x = 240, .y = 216 },
        { .x = 255, .y = 255 },
    }};
    esp_isp_gamma_configure(g_isp_hdl, COLOR_COMPONENT_R, &gamma_pts);
    esp_isp_gamma_configure(g_isp_hdl, COLOR_COMPONENT_G, &gamma_pts);
    esp_isp_gamma_configure(g_isp_hdl, COLOR_COMPONENT_B, &gamma_pts);
    esp_isp_gamma_enable(g_isp_hdl);
    ESP_LOGI(TAG, "ISP Gamma enabled (gamma=1.3 correction)");

    // ── 9. Start CSI (calls on_get_new_trans to obtain the first buffer) ──
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_start(g_cam_hdl),
                        TAG, "CSI start failed");

    // ── 10. NOW start the sensor — MIPI receiver (CSI) and ISP are both ready ──
    int stream_on = 1;
    ESP_RETURN_ON_ERROR(esp_cam_sensor_ioctl(g_sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &stream_on),
                        TAG, "Sensor stream enable failed");

    // ── 11. Allocate H.264 output buffer (PSRAM, manually 64-byte aligned) ──
    g_h264_raw = heap_caps_malloc(g_cap_buf_len + 63, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_h264_raw) {
        ESP_LOGE(TAG, "Failed to alloc H.264 output buffer (%zu bytes)", g_cap_buf_len + 63);
        return ESP_ERR_NO_MEM;
    }
    g_h264_buf = (uint8_t *)(((uintptr_t)g_h264_raw + 63) & ~(uintptr_t)63);

    // ── 12. H.264 HW encoder (VEU) ──
    // ESP32-P4 rev 1.x (chip_rev < 3.0) only supports O_UYY_E_VYY input.
    // enc_fps for rate control: HW encoder takes ~81ms/frame at 800x640, so actual
    // throughput is ~12 fps regardless of sensor speed. Setting RC fps to match
    // reality lets the rate controller allocate the right number of bits per frame.
    // With fps=50 and 1.1Mbps, RC allocated only 22Kbit/frame (P-frames 2-4KB).
    // With fps=15 and 4Mbps, RC allocates ~267Kbit/frame — much better quality.
    uint8_t enc_fps = 15;  // match actual encode throughput, not sensor fps
    g_enc_fps = enc_fps;
    // Scale bitrate: 4 Mbps baseline at 1280×720, scaled by pixel count.
    // Single WiFi AP client (Quest) easily handles 4-6 Mbps.
    uint32_t pixels     = (uint32_t)g_cam_width * g_cam_height;
    uint32_t bitrate    = (uint32_t)(4000000ULL * pixels / (1280 * 720));
    if (bitrate < 1500000) bitrate = 1500000;
    if (bitrate > 8000000) bitrate = 8000000;
    ESP_LOGI(TAG, "H.264 encoder: %ux%u fps=%u gop=%d qp=[20,38] bitrate=%"PRIu32,
             g_cam_width, g_cam_height, enc_fps, ENC_GOP, bitrate);

    esp_h264_enc_cfg_hw_t enc_cfg = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
        .gop      = ENC_GOP,
        .fps      = enc_fps,
        .res      = { .width = g_cam_width, .height = g_cam_height },
        .rc       = { .bitrate = bitrate, .qp_min = 20, .qp_max = 38 },
    };
    esp_h264_err_t h264_ret = esp_h264_enc_hw_new(&enc_cfg, &g_encoder);
    if (h264_ret != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "esp_h264_enc_hw_new failed: %d", (int)h264_ret);
        return ESP_FAIL;
    }
    h264_ret = esp_h264_enc_open(g_encoder);
    if (h264_ret != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "esp_h264_enc_open failed: %d", (int)h264_ret);
        return ESP_FAIL;
    }
    g_cur_bitrate = bitrate;    // track for ABR (camera_get_bitrate)

    // ── 13. Start camera processing task (core 1, stack 8 kB, prio 5) ──
    BaseType_t r = xTaskCreatePinnedToCore(camera_task, "cam_task",
                                           8192, NULL, 5, NULL, 1);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "Failed to create camera task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Camera initialised %ux%u@%ufps — streaming", g_cam_width, g_cam_height, g_enc_fps);
    return ESP_OK;
}

void camera_deinit(void)
{
    if (g_encoder) {
        esp_h264_enc_close(g_encoder);
        esp_h264_enc_del(g_encoder);
        g_encoder = NULL;
    }
    if (g_cam_hdl) {
        esp_cam_ctlr_stop(g_cam_hdl);
        esp_cam_ctlr_disable(g_cam_hdl);
        esp_cam_ctlr_del(g_cam_hdl);
        g_cam_hdl = NULL;
    }
    for (int i = 0; i < NUM_CAP_BUFS; i++) {
        heap_caps_free(g_cap_raw[i]);
        g_cap_raw[i] = NULL;
        g_cap_buf[i]  = NULL;
    }
    heap_caps_free(g_h264_raw);
    g_h264_raw = NULL;
    g_h264_buf = NULL;
    ESP_LOGI(TAG, "camera_deinit done");
}
