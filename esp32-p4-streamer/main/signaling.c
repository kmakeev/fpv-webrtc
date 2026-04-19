#include "signaling.h"
#include "config.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "signaling";

// ──────────────────────────────────────────────
// Globals
// ──────────────────────────────────────────────

static httpd_handle_t               g_server              = NULL;
static int                          g_viewer_fd           = -1;
static signaling_cb_viewer_ready_t  g_cb_viewer_ready     = NULL;
static signaling_cb_answer_t        g_cb_answer           = NULL;
static signaling_cb_ice_t           g_cb_ice              = NULL;
static signaling_cb_disconnected_t  g_cb_peer_disconnected = NULL;

// ──────────────────────────────────────────────
// Callbacks
// ──────────────────────────────────────────────

void signaling_set_callbacks(
    signaling_cb_viewer_ready_t   on_viewer_ready,
    signaling_cb_answer_t         on_answer,
    signaling_cb_ice_t            on_ice,
    signaling_cb_disconnected_t   on_peer_disconnected)
{
    g_cb_viewer_ready      = on_viewer_ready;
    g_cb_answer            = on_answer;
    g_cb_ice               = on_ice;
    g_cb_peer_disconnected = on_peer_disconnected;
}

// ──────────────────────────────────────────────
// WiFi AP (192.168.4.1) — ESP32-C6 co-processor via esp_hosted v2.12.3
// ──────────────────────────────────────────────

static void wifi_init_ap(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = WIFI_AP_SSID,
            .ssid_len       = strlen(WIFI_AP_SSID),
            .password       = WIFI_AP_PASS,
            .channel        = 1,
            .authmode       = WIFI_AUTH_WPA2_PSK,
            .max_connection = 2,
            .beacon_interval= 100,
        },
    };
    if (strlen(WIFI_AP_PASS) == 0) {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP started: SSID=\"%s\" IP=192.168.4.1 port=%d",
             WIFI_AP_SSID, SERVER_PORT);
}

// ──────────────────────────────────────────────
// SPIFFS
// ──────────────────────────────────────────────

static void mount_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = SPIFFS_BASE_PATH,
        .partition_label        = "storage",
        .max_files              = 10,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SPIFFS mounted at %s", SPIFFS_BASE_PATH);
    }
}

// ──────────────────────────────────────────────
// WebSocket — message dispatch
// ──────────────────────────────────────────────

static void handle_ws_message(httpd_req_t *req, const char *json_str)
{
    cJSON *msg = cJSON_Parse(json_str);
    if (!msg) {
        ESP_LOGW(TAG, "Invalid JSON: %s", json_str);
        return;
    }

    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "type"));
    if (!type) { cJSON_Delete(msg); return; }

    if (strcmp(type, "role") == 0) {
        ESP_LOGI(TAG, "Viewer ready");
        if (g_cb_viewer_ready) g_cb_viewer_ready();

    } else if (strcmp(type, "answer") == 0) {
        const char *sdp = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "sdp"));
        if (sdp && g_cb_answer) g_cb_answer(sdp);

    } else if (strcmp(type, "ice") == 0) {
        // candidate field may be an object {candidate, sdpMLineIndex, sdpMid}
        // or a plain string (flat format from some clients)
        cJSON *cand_item = cJSON_GetObjectItem(msg, "candidate");
        const char *candidate = NULL;
        int mline = 0;
        const char *mid = "0";

        if (cJSON_IsObject(cand_item)) {
            candidate = cJSON_GetStringValue(
                            cJSON_GetObjectItem(cand_item, "candidate"));
            cJSON *ml = cJSON_GetObjectItem(cand_item, "sdpMLineIndex");
            if (ml) mline = ml->valueint;
            const char *m = cJSON_GetStringValue(
                                cJSON_GetObjectItem(cand_item, "sdpMid"));
            if (m) mid = m;
        } else if (cJSON_IsString(cand_item)) {
            candidate = cand_item->valuestring;
            cJSON *ml = cJSON_GetObjectItem(msg, "sdpMLineIndex");
            if (ml) mline = ml->valueint;
        }

        if (candidate && g_cb_ice) g_cb_ice(candidate, mline, mid);

    } else {
        ESP_LOGW(TAG, "Unknown message type: %s", type);
    }

    cJSON_Delete(msg);
}

// ──────────────────────────────────────────────
// WebSocket handler
// ──────────────────────────────────────────────

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // Initial WebSocket handshake
        g_viewer_fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "Viewer WebSocket connected, fd=%d", g_viewer_fd);
        return ESP_OK;
    }

    // Receive frame — two-pass: first get length, then allocate + read payload
    httpd_ws_frame_t frame = {0};
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ws_recv_frame (len) failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (frame.len == 0) return ESP_OK;

    uint8_t *buf = malloc(frame.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;

    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret == ESP_OK) {
        buf[frame.len] = '\0';

        if (frame.type == HTTPD_WS_TYPE_CLOSE) {
            ESP_LOGI(TAG, "Viewer sent WS close, fd=%d", g_viewer_fd);
            g_viewer_fd = -1;
            if (g_cb_peer_disconnected) g_cb_peer_disconnected();
        } else if (frame.type == HTTPD_WS_TYPE_TEXT) {
            handle_ws_message(req, (char *)buf);
        }
    } else {
        ESP_LOGE(TAG, "ws_recv_frame (data) failed: %s", esp_err_to_name(ret));
    }

    free(buf);
    return ret;
}

// ──────────────────────────────────────────────
// Static file handler
// ──────────────────────────────────────────────

static esp_err_t static_handler(httpd_req_t *req)
{
    // SPIFFS_BASE_PATH(4) + URI(up to 512 per HTTPD_MAX_URI_LEN) + NUL = 517
    char filepath[520];
    const char *uri = req->uri;

    // / → /index.html
    if (strcmp(uri, "/") == 0) uri = "/index.html";

    snprintf(filepath, sizeof(filepath), "%s%s", SPIFFS_BASE_PATH, uri);

    FILE *f = fopen(filepath, "r");
    if (!f) {
        ESP_LOGW(TAG, "File not found: %s", filepath);
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    // Content-Type by extension
    if (strstr(filepath, ".html")) {
        httpd_resp_set_type(req, "text/html");
    } else if (strstr(filepath, ".js")) {
        httpd_resp_set_type(req, "application/javascript");
    } else if (strstr(filepath, ".css")) {
        httpd_resp_set_type(req, "text/css");
    }

    // Required headers for SharedArrayBuffer in browser
    httpd_resp_set_hdr(req, "Cross-Origin-Embedder-Policy", "require-corp");
    httpd_resp_set_hdr(req, "Cross-Origin-Opener-Policy", "same-origin");

    char chunk[512];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, (ssize_t)n) != ESP_OK) {
            ESP_LOGE(TAG, "Send chunk failed for %s", filepath);
            break;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ──────────────────────────────────────────────
// Thread-safe async send via httpd_queue_work
// ──────────────────────────────────────────────

typedef struct {
    httpd_handle_t hd;
    int            fd;
    char          *payload;
} async_send_t;

static void do_async_send(void *arg)
{
    async_send_t *a = (async_send_t *)arg;
    httpd_ws_frame_t frame = {
        .final   = true,
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)a->payload,
        .len     = strlen(a->payload),
    };
    esp_err_t ret = httpd_ws_send_frame_async(a->hd, a->fd, &frame);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "async send failed: %s", esp_err_to_name(ret));
    }
    free(a->payload);
    free(a);
}

void signaling_send_json(cJSON *msg)
{
    if (!g_server || g_viewer_fd < 0) {
        cJSON_Delete(msg);
        return;
    }
    char *str = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!str) return;

    async_send_t *a = malloc(sizeof(*a));
    if (!a) { free(str); return; }

    a->hd      = g_server;
    a->fd      = g_viewer_fd;
    a->payload = str;
    httpd_queue_work(g_server, do_async_send, a);
}

// ──────────────────────────────────────────────
// Helpers
// ──────────────────────────────────────────────

void signaling_send_offer(const char *sdp)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "offer");
    cJSON_AddStringToObject(msg, "sdp", sdp);
    signaling_send_json(msg);
}

void signaling_send_ice(const char *candidate, int mline, const char *sdp_mid)
{
    cJSON *msg  = cJSON_CreateObject();
    cJSON *cand = cJSON_CreateObject();
    cJSON_AddStringToObject(cand, "candidate", candidate);
    cJSON_AddNumberToObject(cand, "sdpMLineIndex", mline);
    cJSON_AddStringToObject(cand, "sdpMid", sdp_mid ? sdp_mid : "0");
    cJSON_AddItemToObject(msg, "candidate", cand);
    cJSON_AddStringToObject(msg, "type", "ice");
    signaling_send_json(msg);
}

// ──────────────────────────────────────────────
// Disconnect monitor task
// ──────────────────────────────────────────────

static void viewer_monitor_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (g_viewer_fd >= 0 && g_server) {
            httpd_ws_client_info_t info = httpd_ws_get_fd_info(g_server, g_viewer_fd);
            if (info != HTTPD_WS_CLIENT_WEBSOCKET) {
                ESP_LOGI(TAG, "Viewer disconnected (fd=%d, info=%d)",
                         g_viewer_fd, (int)info);
                g_viewer_fd = -1;
                if (g_cb_peer_disconnected) g_cb_peer_disconnected();
            }
        }
    }
}

// ──────────────────────────────────────────────
// Start
// ──────────────────────────────────────────────

esp_err_t signaling_start(uint16_t port)
{
    wifi_init_ap();
    mount_spiffs();

    httpd_config_t config    = HTTPD_DEFAULT_CONFIG();
    config.server_port       = port;
    config.uri_match_fn      = httpd_uri_match_wildcard;

    esp_err_t ret = httpd_start(&g_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // WebSocket handler — both paths (browser uses /, native app uses /ws)
    httpd_uri_t ws_root = {
        .uri          = "/",
        .method       = HTTP_GET,
        .handler      = ws_handler,
        .is_websocket = true,
    };
    httpd_uri_t ws_path = {
        .uri          = "/ws",
        .method       = HTTP_GET,
        .handler      = ws_handler,
        .is_websocket = true,
    };
    httpd_register_uri_handler(g_server, &ws_root);
    httpd_register_uri_handler(g_server, &ws_path);

    // Static file handler — wildcard fallback for all other GET requests
    httpd_uri_t static_files = {
        .uri     = "/*",
        .method  = HTTP_GET,
        .handler = static_handler,
    };
    httpd_register_uri_handler(g_server, &static_files);

    // Background task to detect abrupt disconnections
    xTaskCreate(viewer_monitor_task, "sig_monitor", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "HTTP server started on port %d", port);
    return ESP_OK;
}
