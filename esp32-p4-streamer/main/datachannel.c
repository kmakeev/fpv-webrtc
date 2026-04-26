// PROMPT-05: DataChannel ping/pong clock sync + ts timestamp messages
#include "datachannel.h"
#include "webrtc_streamer.h"
#include "camera.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

static const char *TAG = "datachannel";

// ──────────────────────────────────────────────
// State
// ──────────────────────────────────────────────

// Dedicated task for sending ts messages every 1 s while DataChannel is open.
// Uses a task (not a FreeRTOS timer) so it has its own stack — cJSON printing
// internally calls snprintf/_svfprintf_r which needs ~1 KB stack, far more than
// the 2 KB "Tmr Svc" default allows.
static TaskHandle_t      g_ts_task   = NULL;

// g_encode_ms is updated from camera_task (core 1) and read from the ts task
// (core 0). A portMUX_TYPE spinlock protects it.
static portMUX_TYPE      g_enc_mux   = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t g_encode_ms = 0;

// ──────────────────────────────────────────────
// Helpers
// ──────────────────────────────────────────────

static void dc_send(cJSON *msg)
{
    char *str = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (str) {
        webrtc_datachannel_send(str);
        free(str);
    }
}

// ──────────────────────────────────────────────
// ts task: sends one ts message per second.
// Exits when notified (via datachannel_on_close) or if 1-s delay is interrupted.
// ──────────────────────────────────────────────

static void dc_ts_task(void *arg)
{
    while (1) {
        // Sleep 1 s; wake early if notified to stop
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) != 0) {
            break;  // datachannel_on_close() sent the stop signal
        }

        portENTER_CRITICAL(&g_enc_mux);
        uint32_t enc = g_encode_ms;
        portEXIT_CRITICAL(&g_enc_mux);

        cJSON *msg = cJSON_CreateObject();
        if (!msg) continue;

        // capture = time now (ms since boot).
        // encode  = capture + g_encode_ms so that encode - capture = encode duration.
        double now_ms = (double)(esp_timer_get_time() / 1000);
        cJSON_AddStringToObject(msg, "type",    "ts");
        cJSON_AddNumberToObject(msg, "capture", now_ms);
        cJSON_AddNumberToObject(msg, "encode",  now_ms + (double)enc);
        dc_send(msg);
    }

    g_ts_task = NULL;
    vTaskDelete(NULL);
}

// ──────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────

void datachannel_on_open(void)
{
    ESP_LOGI(TAG, "Channel open");

    if (g_ts_task == NULL) {
        xTaskCreatePinnedToCore(dc_ts_task, "dc_ts", 4096, NULL, 4, &g_ts_task, 0);
        if (!g_ts_task) {
            ESP_LOGE(TAG, "Failed to create dc_ts task");
        }
    }
}

void datachannel_on_message(const char *json_str, size_t len)
{
    if (!json_str || len == 0) return;

    cJSON *msg = cJSON_ParseWithLength(json_str, len);
    if (!msg) {
        ESP_LOGW(TAG, "Invalid JSON on DataChannel");
        return;
    }

    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "type"));
    if (!type) { cJSON_Delete(msg); return; }

    if (strcmp(type, "ping") == 0) {
        cJSON *id_item = cJSON_GetObjectItem(msg, "id");
        cJSON *t0_item = cJSON_GetObjectItem(msg, "t0");
        if (!cJSON_IsNumber(id_item) || !cJSON_IsNumber(t0_item)) {
            cJSON_Delete(msg);
            return;
        }
        int    id = id_item->valueint;
        double t0 = t0_item->valuedouble;
        double t1 = (double)(esp_timer_get_time() / 1000);

        cJSON *pong = cJSON_CreateObject();
        if (pong) {
            cJSON_AddStringToObject(pong, "type", "pong");
            cJSON_AddNumberToObject(pong, "id",   (double)id);
            cJSON_AddNumberToObject(pong, "t0",   t0);
            cJSON_AddNumberToObject(pong, "t1",   t1);
            dc_send(pong);
        }
    } else if (strcmp(type, "pli") == 0) {
        // DataChannel PLI from viewer (freeze recovery) — BYPASS network rate-limit.
        // Network PLI/FIR in webrtc_streamer.c is rate-limited to 1.5 s;
        // DataChannel PLI is used for application-level freeze recovery and has no limit.
        ESP_LOGI(TAG, "DataChannel PLI: forcing IDR (freeze recovery)");
        camera_request_idr();

    } else if (strcmp(type, "resolution") == 0) {
        cJSON *w_item = cJSON_GetObjectItem(msg, "w");
        cJSON *h_item = cJSON_GetObjectItem(msg, "h");
        if (cJSON_IsNumber(w_item) && cJSON_IsNumber(h_item)) {
            uint16_t w = (uint16_t)w_item->valueint;
            uint16_t h = (uint16_t)h_item->valueint;
            ESP_LOGI(TAG, "Resolution switch requested: %ux%u", w, h);
            camera_set_resolution(w, h);
        }
    }
    // Unknown types are silently ignored (e.g. future "head" pose messages)

    cJSON_Delete(msg);
}

void datachannel_on_close(void)
{
    ESP_LOGI(TAG, "Channel closed");
    if (g_ts_task) {
        xTaskNotifyGive(g_ts_task);  // wake dc_ts_task to exit
        // Task deletes itself; g_ts_task cleared by the task itself
    }
}

void datachannel_update_encode_ms(uint32_t encode_ms)
{
    portENTER_CRITICAL(&g_enc_mux);
    g_encode_ms = encode_ms;
    portEXIT_CRITICAL(&g_enc_mux);
}
