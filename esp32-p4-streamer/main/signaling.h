#pragma once

#include "cJSON.h"
#include "esp_err.h"

// Callbacks — set before signaling_start()
typedef void (*signaling_cb_viewer_ready_t)(void);
typedef void (*signaling_cb_answer_t)(const char *sdp);
typedef void (*signaling_cb_ice_t)(const char *candidate,
                                   int sdp_mline_index,
                                   const char *sdp_mid);
typedef void (*signaling_cb_disconnected_t)(void);

void signaling_set_callbacks(
    signaling_cb_viewer_ready_t   on_viewer_ready,
    signaling_cb_answer_t         on_answer,
    signaling_cb_ice_t            on_ice,
    signaling_cb_disconnected_t   on_peer_disconnected
);

// Init: mount SPIFFS, start WiFi AP + HTTP server
esp_err_t signaling_start(uint16_t port);

// Thread-safe JSON send to viewer (from any FreeRTOS context)
void signaling_send_json(cJSON *msg);

// Helpers
void signaling_send_offer(const char *sdp);
void signaling_send_ice(const char *candidate, int mline_index, const char *sdp_mid);
