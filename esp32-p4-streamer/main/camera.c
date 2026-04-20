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

// ──────────────────────────────────────────────
// Camera + encode task  (pinned to core 1)
// ──────────────────────────────────────────────

void camera_request_idr(void)
{
    g_force_idr = true;
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

    while (1) {
        if (xQueueReceive(g_frame_q, &frame_buf, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        task_frame_count++;
        // Heartbeat: first frame + every 30 frames
        if (task_frame_count == 1 || task_frame_count % 30 == 0) {
            ESP_LOGI(TAG, "camera_task: frame #%"PRIu32, task_frame_count);
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

            // After a forced IDR (or any IDR while GOP != g_enc_fps), restore GOP.
            if (out_frame.frame_type == ESP_H264_FRAME_TYPE_IDR) {
                esp_h264_enc_param_handle_t ph = s_get_param_hd();
                if (ph) {
                    uint8_t cur_gop = 0;
                    esp_h264_enc_get_gop(ph, &cur_gop);
                    if (cur_gop != g_enc_fps) {
                        esp_h264_enc_set_gop(ph, g_enc_fps);
                        ESP_LOGI(TAG, "IDR sent — GOP restored to %d", (int)g_enc_fps);
                    }
                }
            }

            if (g_frame_cb) {
                g_frame_cb(out_frame.raw_data.buffer, (size_t)out_frame.length,
                           capture_ts, encode_ms);
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
    esp_cam_sensor_query_format(sensor, &fmt_arr);

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

    ESP_RETURN_ON_ERROR(esp_cam_sensor_set_format(sensor, selected),
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
    isp_proc_handle_t isp_hdl = NULL;
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
    ESP_RETURN_ON_ERROR(esp_isp_new_processor(&isp_cfg, &isp_hdl),
                        TAG, "ISP init failed");
    ESP_RETURN_ON_ERROR(esp_isp_enable(isp_hdl),
                        TAG, "ISP enable failed");

    // ── 9. Start CSI (calls on_get_new_trans to obtain the first buffer) ──
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_start(g_cam_hdl),
                        TAG, "CSI start failed");

    // ── 10. NOW start the sensor — MIPI receiver (CSI) and ISP are both ready ──
    int stream_on = 1;
    ESP_RETURN_ON_ERROR(esp_cam_sensor_ioctl(sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &stream_on),
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
    // Use sensor fps for GOP interval so keyframe period stays at 1 second.
    uint8_t enc_fps = selected->fps > 0 ? selected->fps : CAM_FPS;
    g_enc_fps = enc_fps;
    // Scale bitrate with pixel count relative to 1280×720 baseline at 2 Mbps.
    uint32_t pixels     = (uint32_t)g_cam_width * g_cam_height;
    uint32_t bitrate    = (uint32_t)(2000000ULL * pixels / (1280 * 720));
    if (bitrate < 1000000) bitrate = 1000000;
    if (bitrate > 8000000) bitrate = 8000000;
    ESP_LOGI(TAG, "H.264 encoder: %ux%u fps=%u gop=%u bitrate=%"PRIu32,
             g_cam_width, g_cam_height, enc_fps, enc_fps, bitrate);

    esp_h264_enc_cfg_hw_t enc_cfg = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
        .gop      = enc_fps,
        .fps      = enc_fps,
        .res      = { .width = g_cam_width, .height = g_cam_height },
        .rc       = { .bitrate = bitrate, .qp_min = 25, .qp_max = 45 },
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
