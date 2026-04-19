// PROMPT-04: Camera OV5647 MIPI CSI + hardware H.264 encoder (VEU)
// Native IDF stack: esp_driver_cam + esp_driver_isp + espressif/esp_cam_sensor
//
// Pipeline:
//   OV5647 → MIPI CSI (RAW8) → ESP ISP (RAW8→YUV420) → PSRAM buffer
//   → I420→O_UYY_E_VYY conversion → H.264 HW VEU encoder → frame_cb
//
// Double-buffer: buf[0] / buf[1] alternate as DMA target.
// Processing window = one frame time (~33 ms @ 30 fps); measured encode ≈ 15 ms.

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
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_ctlr.h"
#include "esp_sccb_intf.h"
#include "esp_sccb_i2c.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_h264_enc_single.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <inttypes.h>
#include <string.h>

static const char *TAG = "camera";

// ──────────────────────────────────────────────
// Board-specific constants (ESP32-P4-Function-EV-Board)
// ──────────────────────────────────────────────

// OV5647 SCCB (I2C) pins
#define CAM_SCCB_SDA_IO       7
#define CAM_SCCB_SCL_IO       8
#define CAM_SCCB_FREQ_HZ      100000

// MIPI PHY LDO (powers the MIPI CSI receiver on ESP32-P4)
#define CAM_LDO_CHAN_ID       3
#define CAM_LDO_VOLTAGE_MV    2500

// MIPI CSI: 2 data lanes, 200 Mbps/lane (OV5647 720p30 RAW8 mode)
#define CSI_DATA_LANES        2
#define CSI_LANE_MBPS         200

// ──────────────────────────────────────────────
// Buffer sizes
// ──────────────────────────────────────────────

// ISP output: YUV420 planar (I420), 1.5 bytes/pixel
#define CAM_CAP_BUF_LEN  ((size_t)(CAM_WIDTH) * (CAM_HEIGHT) * 3 / 2)
// H.264 output: worst case ≤ uncompressed frame size
#define CAM_H264_BUF_LEN CAM_CAP_BUF_LEN
// Number of capture (DMA) buffers: double-buffered
#define NUM_CAP_BUFS     2

// ──────────────────────────────────────────────
// Module state
// ──────────────────────────────────────────────

static camera_frame_cb_t      g_frame_cb  = NULL;
static esp_cam_ctlr_handle_t  g_cam_hdl   = NULL;
static esp_h264_enc_handle_t  g_encoder   = NULL;

// Capture buffers (PSRAM, 16-byte aligned for DMA + H.264 encoder)
static uint8_t  *g_cap_buf[NUM_CAP_BUFS];
// Ring index: index of the buffer to use for the NEXT DMA transfer
static volatile int g_dma_next = 0;

// Queue: ISR → camera_task. Item = pointer to the just-captured PSRAM buffer.
// Depth 1 + overwrite: always has the latest frame; drops old if task is slow.
static QueueHandle_t g_frame_q = NULL;

// Conversion buffer: ISP YUV420 (I420) → O_UYY_E_VYY expected by HW encoder
static uint8_t  *g_yuv_buf  = NULL;
// H.264 bitstream output buffer
static uint8_t  *g_h264_buf = NULL;

// ──────────────────────────────────────────────
// Format conversion: I420 (YUV420 planar) → O_UYY_E_VYY
// ──────────────────────────────────────────────
//
// I420 layout:
//   [Y  plane: W*H bytes]
//   [U  plane: W/2 * H/2 bytes]
//   [V  plane: W/2 * H/2 bytes]
//
// O_UYY_E_VYY (ESP32-P4 HW H.264, rev 1.x only):
//   odd  rows: U Y0 Y1 | U Y2 Y3 | ...    (3 bytes per 2-pixel group)
//   even rows: V Y0 Y1 | V Y2 Y3 | ...
//
// Both are 1.5 bytes/pixel total.
//
static void i420_to_ouyy_evyy(const uint8_t *i420, uint8_t *dst,
                               int width, int height)
{
    const uint8_t *Y = i420;
    const uint8_t *U = Y + width * height;
    const uint8_t *V = U + (width / 2) * (height / 2);

    for (int row = 0; row < height; row++) {
        const uint8_t *y_row  = Y + row * width;
        int            uv_row = row / 2;
        // Odd rows use U, even rows use V (O_UYY = odd-U, E_VYY = even-V)
        const uint8_t *chroma = (row & 1)
                                ? U + uv_row * (width / 2)
                                : V + uv_row * (width / 2);
        for (int col = 0; col < width / 2; col++) {
            *dst++ = chroma[col];
            *dst++ = y_row[col * 2];
            *dst++ = y_row[col * 2 + 1];
        }
    }
}

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
    trans->buflen = CAM_CAP_BUF_LEN;
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

// ──────────────────────────────────────────────
// Camera + encode task  (pinned to core 1)
// ──────────────────────────────────────────────

static void camera_task(void *arg)
{
    bool first_frame = true;
    const uint8_t *frame_buf = NULL;

    while (1) {
        if (xQueueReceive(g_frame_q, &frame_buf, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        uint64_t capture_ts = (uint64_t)(esp_timer_get_time() / 1000);
        uint64_t t0         = esp_timer_get_time();

        // Convert ISP output (I420) → O_UYY_E_VYY required by HW encoder
        i420_to_ouyy_evyy(frame_buf, g_yuv_buf, CAM_WIDTH, CAM_HEIGHT);

        // Encode with hardware H.264 VEU
        esp_h264_enc_in_frame_t in_frame = {
            .raw_data = { .buffer = g_yuv_buf,  .len = (uint32_t)CAM_CAP_BUF_LEN },
        };
        esp_h264_enc_out_frame_t out_frame = {
            .raw_data = { .buffer = g_h264_buf, .len = (uint32_t)CAM_H264_BUF_LEN },
        };

        esp_h264_err_t ret = esp_h264_enc_process(g_encoder, &in_frame, &out_frame);
        if (ret == ESP_H264_ERR_OK && out_frame.length > 0) {
            uint32_t encode_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
            if (first_frame) {
                ESP_LOGI(TAG, "First H.264 frame: %"PRIu32" bytes, encode=%"PRIu32" ms",
                         out_frame.length, encode_ms);
                first_frame = false;
            }
            if (g_frame_cb) {
                g_frame_cb(out_frame.raw_data.buffer, (size_t)out_frame.length,
                           capture_ts, encode_ms);
            }
        } else if (ret != ESP_H264_ERR_OK) {
            ESP_LOGW(TAG, "H.264 enc_process error: %d", (int)ret);
        }
    }
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

    // ── 3. Auto-detect camera sensor ──
    esp_cam_sensor_config_t sensor_cfg = {
        .reset_pin   = -1,
        .pwdn_pin    = -1,
        .xclk_pin    = -1,
    };
    esp_cam_sensor_device_t *sensor = NULL;

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
        sensor = (*(p->detect))(&sensor_cfg);
        if (sensor) {
            ESP_LOGI(TAG, "Camera sensor detected");
            break;
        }
        esp_sccb_del_i2c_io(sensor_cfg.sccb_handle);
    }
    if (!sensor) {
        ESP_LOGE(TAG, "No camera sensor detected on I2C bus (SDA=%d SCL=%d)",
                 CAM_SCCB_SDA_IO, CAM_SCCB_SCL_IO);
        return ESP_ERR_NOT_FOUND;
    }

    // ── 4. Select 1280×720 sensor format ──
    esp_cam_sensor_format_array_t fmt_arr = {0};
    esp_cam_sensor_query_format(sensor, &fmt_arr);

    const esp_cam_sensor_format_t *selected = NULL;
    for (int i = 0; i < (int)fmt_arr.count; i++) {
        ESP_LOGI(TAG, "Sensor fmt[%d]: %s", i, fmt_arr.format_array[i].name);
        // Select first format whose name contains our target resolution
        if (!selected && strstr(fmt_arr.format_array[i].name, "1280x720")) {
            selected = &fmt_arr.format_array[i];
        }
    }
    if (!selected) {
        ESP_LOGW(TAG, "No 1280x720 format found — using first available format");
        if (fmt_arr.count == 0) {
            ESP_LOGE(TAG, "Sensor reports no formats");
            return ESP_FAIL;
        }
        selected = &fmt_arr.format_array[0];
    }
    ESP_LOGI(TAG, "Using sensor format: %s", selected->name);

    ESP_RETURN_ON_ERROR(esp_cam_sensor_set_format(sensor, selected),
                        TAG, "Sensor set_format failed");

    int stream_on = 1;
    ESP_RETURN_ON_ERROR(esp_cam_sensor_ioctl(sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &stream_on),
                        TAG, "Sensor stream enable failed");

    // ── 5. Allocate capture buffers (2×, PSRAM, 16-byte aligned) ──
    for (int i = 0; i < NUM_CAP_BUFS; i++) {
        g_cap_buf[i] = heap_caps_aligned_alloc(64, CAM_CAP_BUF_LEN,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!g_cap_buf[i]) {
            ESP_LOGE(TAG, "Failed to alloc capture buffer %d (%zu bytes)",
                     i, CAM_CAP_BUF_LEN);
            return ESP_ERR_NO_MEM;
        }
    }

    // ── 6. Create frame-ready queue (depth 1, overwrite on full) ──
    g_frame_q = xQueueCreate(1, sizeof(uint8_t *));
    if (!g_frame_q) {
        return ESP_ERR_NO_MEM;
    }

    // ── 7. CSI controller: RAW8 input → YUV420 output (ISP converts) ──
    esp_cam_ctlr_csi_config_t csi_cfg = {
        .ctlr_id               = 0,
        .h_res                 = CAM_WIDTH,
        .v_res                 = CAM_HEIGHT,
        .data_lane_num         = CSI_DATA_LANES,
        .lane_bit_rate_mbps    = CSI_LANE_MBPS,
        .input_data_color_type = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type= CAM_CTLR_COLOR_YUV420,
        .queue_items           = 1,
        .byte_swap_en          = false,
        .bk_buffer_dis         = true,   // use our own PSRAM buffers
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

    // ── 8. ISP: RAW8 → YUV420 ──
    isp_proc_handle_t isp_hdl = NULL;
    esp_isp_processor_cfg_t isp_cfg = {
        .clk_hz                 = 80 * 1000 * 1000,
        .input_data_source      = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type  = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_YUV420,
        .has_line_start_packet  = false,
        .has_line_end_packet    = false,
        .h_res                  = CAM_WIDTH,
        .v_res                  = CAM_HEIGHT,
    };
    ESP_RETURN_ON_ERROR(esp_isp_new_processor(&isp_cfg, &isp_hdl),
                        TAG, "ISP init failed");
    ESP_RETURN_ON_ERROR(esp_isp_enable(isp_hdl),
                        TAG, "ISP enable failed");

    // ── 9. Start CSI (calls on_get_new_trans to obtain the first buffer) ──
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_start(g_cam_hdl),
                        TAG, "CSI start failed");

    // ── 10. Allocate conversion and H.264 output buffers (PSRAM) ──
    g_yuv_buf  = heap_caps_aligned_alloc(16, CAM_CAP_BUF_LEN,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    g_h264_buf = heap_caps_aligned_alloc(16, CAM_H264_BUF_LEN,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_yuv_buf || !g_h264_buf) {
        ESP_LOGE(TAG, "Failed to alloc PSRAM processing buffers (%zu bytes each)",
                 CAM_CAP_BUF_LEN);
        return ESP_ERR_NO_MEM;
    }

    // ── 11. H.264 HW encoder (VEU) ──
    // ESP32-P4 rev 1.x (chip_rev < 3.0) only supports O_UYY_E_VYY input.
    esp_h264_enc_cfg_hw_t enc_cfg = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
        .gop      = CAM_FPS,     // I-frame every 30 frames
        .fps      = CAM_FPS,
        .res      = { .width = CAM_WIDTH, .height = CAM_HEIGHT },
        .rc       = { .bitrate = 2000000, .qp_min = 25, .qp_max = 45 },
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

    // ── 12. Start camera processing task (core 1, stack 8 kB, prio 5) ──
    BaseType_t r = xTaskCreatePinnedToCore(camera_task, "cam_task",
                                           8192, NULL, 5, NULL, 1);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "Failed to create camera task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OV5647 initialised %dx%d@%dfps — streaming", CAM_WIDTH, CAM_HEIGHT, CAM_FPS);
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
        heap_caps_free(g_cap_buf[i]);
        g_cap_buf[i] = NULL;
    }
    heap_caps_free(g_yuv_buf);
    heap_caps_free(g_h264_buf);
    g_yuv_buf  = NULL;
    g_h264_buf = NULL;
    ESP_LOGI(TAG, "camera_deinit done");
}
