#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "config.h"
#include "signaling.h"
#include "webrtc_streamer.h"
#include "camera.h"
#include "datachannel.h"

static const char *TAG = "main";

// ──────────────────────────────────────────────
// WebRTC callbacks
// ──────────────────────────────────────────────

static void on_webrtc_connected(void)
{
    ESP_LOGI(TAG, "WebRTC connected — streaming");
    // TODO PROMPT-05: datachannel_on_open();
}

static void on_webrtc_disconnected(void)
{
    ESP_LOGI(TAG, "WebRTC disconnected");
    // TODO PROMPT-05: datachannel_on_close();
}

static void on_datachannel_msg(const char *msg, size_t len)
{
    // TODO PROMPT-05: datachannel_on_message(msg, len);
    (void)msg;
    (void)len;
}

// ──────────────────────────────────────────────
// Camera callback (PROMPT-04)
// ──────────────────────────────────────────────

static void on_camera_frame(const uint8_t *data, size_t len,
                            uint64_t pts_ms, uint32_t encode_ms)
{
    // TODO PROMPT-05: datachannel_update_encode_ms(encode_ms);
    (void)encode_ms;
    webrtc_push_video_frame(data, len, pts_ms);
}

// ──────────────────────────────────────────────
// Signaling callbacks
// ──────────────────────────────────────────────

static void on_viewer_ready(void)
{
    ESP_LOGI(TAG, "Viewer connected, creating offer...");
    webrtc_reset();         // tear down any previous session
    webrtc_create_offer();  // → on_ice_candidate_cb → signaling_send_offer
}

static void on_peer_disconnected(void)
{
    ESP_LOGI(TAG, "Viewer disconnected, resetting WebRTC");
    // TODO PROMPT-05: datachannel_on_close();
    webrtc_reset();
}

// ──────────────────────────────────────────────
// Entry point
// ──────────────────────────────────────────────

void app_main(void)
{
    ESP_LOGI(TAG, "FPV ESP32-P4 Streamer starting...");

    // NVS — required by WiFi
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // Network interface + event loop — required before WiFi init
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // PROMPT-03: WebRTC (init before signaling — will be called from on_viewer_ready)
    webrtc_set_callbacks(on_webrtc_connected, on_webrtc_disconnected, on_datachannel_msg);
    webrtc_init();

    // TODO PROMPT-04: camera_init(on_camera_frame);
    (void)on_camera_frame; // suppress unused warning until PROMPT-04

    // PROMPT-02: WiFi AP + HTTP/WebSocket signaling server
    signaling_set_callbacks(on_viewer_ready,
                            webrtc_set_answer,
                            webrtc_add_ice_candidate,
                            on_peer_disconnected);
    ESP_ERROR_CHECK(signaling_start(SERVER_PORT));
    ESP_LOGI(TAG, "Ready — waiting for viewer on ws://192.168.4.1:%d/ws", SERVER_PORT);

    // All further work is event-driven (callbacks + FreeRTOS tasks)
    vTaskDelay(portMAX_DELAY);
}
