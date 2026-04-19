// PROMPT-05: DataChannel ping/pong clock sync + ts timestamp messages
#include "datachannel.h"
#include "webrtc_streamer.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/portmacro.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

static const char *TAG = "datachannel";

// ──────────────────────────────────────────────
// State
// ──────────────────────────────────────────────

static TimerHandle_t     g_ts_timer  = NULL;

// g_encode_ms is updated from camera_task (core 1) and read from the timer
// callback (timer task / core 0). A portMUX_TYPE spinlock protects it.
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
// ts-timer callback: fires every 1000 ms while DataChannel is open
// ──────────────────────────────────────────────

static void ts_timer_cb(TimerHandle_t xTimer)
{
    portENTER_CRITICAL(&g_enc_mux);
    uint32_t enc = g_encode_ms;
    portEXIT_CRITICAL(&g_enc_mux);

    cJSON *msg = cJSON_CreateObject();
    if (!msg) return;

    cJSON_AddStringToObject(msg, "type",    "ts");
    cJSON_AddNumberToObject(msg, "capture", (double)(esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(msg, "encode",  (double)enc);

    ESP_LOGD(TAG, "Sent ts: capture=... encode=%"PRIu32"ms", enc);
    dc_send(msg);
}

// ──────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────

void datachannel_on_open(void)
{
    ESP_LOGI(TAG, "Channel open");

    // Create (or restart) the 1-second ts timer
    if (g_ts_timer == NULL) {
        g_ts_timer = xTimerCreate("dc_ts",
                                  pdMS_TO_TICKS(1000),
                                  pdTRUE,       // auto-reload
                                  NULL,
                                  ts_timer_cb);
    }
    if (g_ts_timer) {
        xTimerStart(g_ts_timer, 0);
    } else {
        ESP_LOGE(TAG, "Failed to create ts timer");
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

        ESP_LOGD(TAG, "ping id=%d t0=%.0f → pong t1=%.0f", id, t0, t1);

        cJSON *pong = cJSON_CreateObject();
        if (pong) {
            cJSON_AddStringToObject(pong, "type", "pong");
            cJSON_AddNumberToObject(pong, "id",   (double)id);
            cJSON_AddNumberToObject(pong, "t0",   t0);
            cJSON_AddNumberToObject(pong, "t1",   t1);
            dc_send(pong);
        }
    }
    // Unknown types are silently ignored (e.g. future "head" pose messages)

    cJSON_Delete(msg);
}

void datachannel_on_close(void)
{
    ESP_LOGI(TAG, "Channel closed");
    if (g_ts_timer) {
        xTimerStop(g_ts_timer, 0);
        xTimerDelete(g_ts_timer, 0);
        g_ts_timer = NULL;
    }
}

void datachannel_update_encode_ms(uint32_t encode_ms)
{
    portENTER_CRITICAL(&g_enc_mux);
    g_encode_ms = encode_ms;
    portEXIT_CRITICAL(&g_enc_mux);
}
