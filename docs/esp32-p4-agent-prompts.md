# ESP32-P4 FPV Streamer — промты для AI-агента

Документ содержит 6 последовательных промтов для AI-агента (Claude Code).
Выполняйте строго по порядку: каждый промт зависит от результата предыдущего.

**Архитектура:** ESP32-P4 работает полностью автономно — поднимает собственную WiFi точку
доступа ("FPV-Drone"), на борту запускает HTTP + WebSocket сервер для сигналинга и раздачи
веб-страницы. Quest 2 подключается к WiFi ESP32 и открывает `http://192.168.4.1/`.
Ноутбук нужен только для первоначальной сборки и прошивки.

**Контекст проекта (вставлять в начало каждого промта):**
> Репозиторий: `/Users/konstantin/cursor/fpv-webrtc`
> Задача: создать прошивку ESP32-P4-Function-EV-Board с камерой OV5647. Плата поднимает
> WiFi AP "FPV-Drone", на борту запускает HTTP + WebSocket сервер (порт 8080, путь `/ws`),
> раздаёт веб-страницу из SPIFFS (`/www` → файлы из `public/`) и выступает WebRTC-стримером.
> Сигналинг полностью на борту — никакого Node.js-сервера не нужно.

---

## PROMPT-01 — Создание структуры ESP-IDF проекта

```
Создай ESP-IDF проект для ESP32-P4 в директории `esp32-p4-streamer/` в корне репозитория.

### Контекст

ESP32-P4-Function-EV-Board работает полностью автономно:
- WiFi в режиме точки доступа (AP), SSID "FPV-Drone", IP 192.168.4.1
- HTTP-сервер (порт 8080): раздаёт файлы из public/ через SPIFFS
- WebSocket-сервер (путь /ws): сигналинг SDP/ICE с Quest 2
- WebRTC-стример: H.264 с OV5647 → Quest 2

### Структура файлов для создания

```
esp32-p4-streamer/
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions.csv
├── idf_component.yml
└── main/
    ├── CMakeLists.txt
    ├── config.h
    ├── main.c
    ├── signaling.h      (пустой заголовок — заполним в PROMPT-02)
    ├── signaling.c      (пустой stub)
    ├── webrtc_streamer.h
    ├── webrtc_streamer.c
    ├── camera.h
    ├── camera.c
    ├── datachannel.h
    └── datachannel.c
```

### Требования к каждому файлу

**esp32-p4-streamer/CMakeLists.txt:**
```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(fpv_streamer)

# SPIFFS-образ из директории public/ — монтируется как /www в прошивке
spiffs_create_partition_image(storage ../public FLASH_IN_PROJECT)
```

**esp32-p4-streamer/sdkconfig.defaults:**
```
CONFIG_IDF_TARGET="esp32p4"
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=16
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=64
CONFIG_ESP_WIFI_AMPDU_TX_ENABLED=y
CONFIG_ESP_WIFI_AMPDU_RX_ENABLED=y
CONFIG_LWIP_TCP_MSS=1436
CONFIG_LWIP_TCPIP_TASK_STACK_SIZE=4096
CONFIG_FREERTOS_HZ=1000
CONFIG_ESP_TASK_WDT_TIMEOUT_S=30
CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
```

**esp32-p4-streamer/partitions.csv:**
```csv
# Name,   Type, SubType, Offset,   Size,    Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  0x400000,
app1,     app,  ota_1,   0x410000, 0x400000,
storage,  data, spiffs,  0x810000, 0x200000,
```

**esp32-p4-streamer/idf_component.yml:**
```yaml
## IDF Component Manager Manifest File
dependencies:
  idf: ">=5.3"
  espressif/esp_camera: "*"
  espressif/esp_h264: "*"
  espressif/libpeer: "*"
```

> Примечание: esp_http_server и spiffs входят в ESP-IDF и не требуют отдельных
> зависимостей. esp_websocket_client НЕ нужен — ESP32 сам является сервером.

**esp32-p4-streamer/main/CMakeLists.txt:**
```cmake
idf_component_register(
    SRCS
        "main.c"
        "signaling.c"
        "webrtc_streamer.c"
        "camera.c"
        "datachannel.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        spiffs
        esp_camera
        esp_h264
        cJSON
        freertos
        log
)
```

**esp32-p4-streamer/main/config.h:**
```c
#pragma once

// WiFi Access Point — ESP32-P4 создаёт собственную сеть
#define WIFI_AP_SSID        "FPV-Drone"
#define WIFI_AP_PASS        ""          // пустая строка = открытая сеть
#define WIFI_AP_CHANNEL     6
#define WIFI_AP_MAX_CONN    2

// HTTP + WebSocket сервер (запускается на ESP32, IP 192.168.4.1)
#define SERVER_PORT         8080
#define WS_PATH             "/ws"
#define SPIFFS_BASE_PATH    "/www"

// Параметры камеры
#define CAM_WIDTH           1280
#define CAM_HEIGHT          720
#define CAM_FPS             30

// Тег для логирования
#define TAG                 "FPV"
```

**esp32-p4-streamer/main/main.c** — скелет с заглушками:
```c
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "config.h"
#include "signaling.h"
#include "webrtc_streamer.h"
#include "camera.h"
#include "datachannel.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "FPV ESP32-P4 Streamer starting...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // TODO: будет заполнено в PROMPT-02..06
    ESP_LOGI(TAG, "Stub — see subsequent prompts");
    vTaskDelay(portMAX_DELAY);
}
```

Для всех остальных .c и .h файлов создай минимальные заглушки с комментарием
"// TODO: implemented in PROMPT-0N" чтобы проект компилировался.

### Проверка

```bash
cd esp32-p4-streamer
idf.py set-target esp32p4
idf.py build 2>&1 | tail -20
```
Сборка должна завершиться без ошибок (предупреждения допустимы).
```

---

## PROMPT-02 — WiFi AP + HTTP/WebSocket сервер (сигналинг)

```
Реализуй модуль сигналинга `signaling.c` / `signaling.h` для ESP32-P4 FPV стримера.

### Контекст

В отличие от браузерного streamer.html (который подключался к внешнему серверу),
ESP32 САМА является сервером. Никакого Node.js server.js нет — вся логика сигналинга
встроена в прошивку.

Прочитай файлы для справки:
- `server/server.js` — оригинальный Node.js сервер, логику которого мы переносим на ESP32
- `public/js/webrtc-client.js` — поведение viewer (Quest 2), с которым будет общаться ESP32
- `fpv-native-quest/app/src/main/java/com/fpv/quest/SignalingClient.kt` — нативный клиент Quest

Протокол WebSocket (со стороны Quest viewer):
1. Viewer подключается к `ws://192.168.4.1:8080/ws`
2. Viewer → ESP32: `{"type":"role","role":"viewer"}`
3. ESP32 → Viewer: `{"type":"offer","sdp":"v=0\r\n..."}`
4. Viewer → ESP32: `{"type":"answer","sdp":"v=0\r\n..."}`
5. Обмен ICE-кандидатами в обе стороны: `{"type":"ice","candidate":{...}}`
6. При разрыве соединения: `g_viewer_fd = -1`, сброс WebRTC

Дополнительно ESP32 раздаёт статические файлы из SPIFFS (/www) через HTTP:
- `GET /` → `/www/index.html`
- `GET /js/*.js`, `GET /css/*.css` → соответствующие файлы из /www/

### Требования к signaling.h

```c
#pragma once
#include "cJSON.h"
#include "esp_err.h"

// Колбэки — устанавливать до signaling_start()
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

// Инициализация: монтирует SPIFFS, запускает WiFi AP + HTTP-сервер
esp_err_t signaling_start(uint16_t port);

// Thread-safe отправка JSON viewer-у (из любого FreeRTOS-контекста)
void signaling_send_json(cJSON *msg);

// Хелперы
void signaling_send_offer(const char *sdp);
void signaling_send_ice(const char *candidate, int mline_index, const char *sdp_mid);
```

### Требования к signaling.c

#### 1. WiFi AP инициализация (`wifi_init_ap`)

```c
static void wifi_init_ap(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t ap_config = {
        .ap = {
            .ssid           = WIFI_AP_SSID,
            .ssid_len       = strlen(WIFI_AP_SSID),
            .channel        = WIFI_AP_CHANNEL,
            .password       = WIFI_AP_PASS,
            .max_connection = WIFI_AP_MAX_CONN,
            // Если пароль пустой — open network:
            .authmode       = strlen(WIFI_AP_PASS) == 0
                              ? WIFI_AUTH_OPEN
                              : WIFI_AUTH_WPA2_PSK,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "WiFi AP started: SSID=%s, IP=192.168.4.1, port=%d",
             WIFI_AP_SSID, SERVER_PORT);
}
```

#### 2. SPIFFS монтирование

```c
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
```

#### 3. HTTP-сервер со статикой и WebSocket

Зарегистрировать два URI-обработчика:

**3a. WebSocket на `/` и `/ws` (оба пути)**

Браузерный viewer (`public/js/main.js:43`) строит URL из `window.location.origin` без
явного пути: `ws://192.168.4.1:8080/`. Нативное приложение Quest 2 использует
`ws://192.168.4.1:8080/ws`. Чтобы оба работали, зарегистрировать WS-хэндлер дважды:
```c
// Один и тот же хэндлер, два URI
httpd_uri_t ws_root = { .uri="/",   .method=HTTP_GET, .handler=ws_handler, .is_websocket=true };
httpd_uri_t ws_path = { .uri="/ws", .method=HTTP_GET, .handler=ws_handler, .is_websocket=true };
httpd_register_uri_handler(server, &ws_root);
httpd_register_uri_handler(server, &ws_path);
```

**WebSocket хэндлер (`ws_handler`):**

```c
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // Начальный WebSocket handshake
        g_viewer_fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "Viewer WebSocket connected, fd=%d", g_viewer_fd);
        return ESP_OK;
    }

    // Получение фрейма (динамическая аллокация для больших SDP)
    httpd_ws_frame_t frame = {0};
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK || frame.len == 0) return ret;

    uint8_t *buf = malloc(frame.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret == ESP_OK && frame.type == HTTPD_WS_TYPE_TEXT) {
        buf[frame.len] = '\0';
        handle_ws_message(req, (char *)buf);
    }
    free(buf);
    return ret;
}

// Обработка JSON-сообщений от viewer:
static void handle_ws_message(httpd_req_t *req, const char *json_str)
{
    cJSON *msg = cJSON_Parse(json_str);
    if (!msg) return;

    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "type"));
    if (!type) { cJSON_Delete(msg); return; }

    if (strcmp(type, "role") == 0) {
        // Viewer готов — инициировать WebRTC offer
        if (g_cb_viewer_ready) g_cb_viewer_ready();

    } else if (strcmp(type, "answer") == 0) {
        const char *sdp = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "sdp"));
        if (sdp && g_cb_answer) g_cb_answer(sdp);

    } else if (strcmp(type, "ice") == 0) {
        // candidate может быть объектом или строкой (см. streamer.html:94-103)
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
    }
    cJSON_Delete(msg);
}
```

**3b. Статические файлы (`/*`):**

```c
static esp_err_t static_handler(httpd_req_t *req)
{
    char filepath[128];
    const char *uri = req->uri;

    // / → /www/index.html
    if (strcmp(uri, "/") == 0) uri = "/index.html";

    snprintf(filepath, sizeof(filepath), "%s%s", SPIFFS_BASE_PATH, uri);

    FILE *f = fopen(filepath, "r");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    // Content-Type по расширению
    if (strstr(filepath, ".html")) httpd_resp_set_type(req, "text/html");
    else if (strstr(filepath, ".js")) httpd_resp_set_type(req, "application/javascript");
    else if (strstr(filepath, ".css")) httpd_resp_set_type(req, "text/css");

    // Добавить COEP/COOP заголовки (нужны для SharedArrayBuffer в браузере)
    httpd_resp_set_hdr(req, "Cross-Origin-Embedder-Policy", "require-corp");
    httpd_resp_set_hdr(req, "Cross-Origin-Opener-Policy", "same-origin");

    char chunk[512];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        httpd_resp_send_chunk(req, chunk, n);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}
```

#### 4. Thread-safe отправка через `httpd_queue_work`

Важно: `signaling_send_json` может вызываться из любого FreeRTOS-таска (WebRTC, DataChannel).
`httpd_queue_work` — единственный безопасный способ отправить WS-фрейм из внешнего контекста.

```c
typedef struct {
    httpd_handle_t hd;
    int            fd;
    char          *payload;
} async_send_t;

static void do_async_send(void *arg)
{
    async_send_t *a = (async_send_t *)arg;
    httpd_ws_frame_t frame = {
        .final     = true,
        .type      = HTTPD_WS_TYPE_TEXT,
        .payload   = (uint8_t *)a->payload,
        .len       = strlen(a->payload),
    };
    httpd_ws_send_frame_async(a->hd, a->fd, &frame);
    free(a->payload);
    free(a);
}

void signaling_send_json(cJSON *msg)
{
    if (!g_server || g_viewer_fd < 0) { cJSON_Delete(msg); return; }
    char *str = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    async_send_t *a = malloc(sizeof(*a));
    a->hd = g_server;
    a->fd = g_viewer_fd;
    a->payload = str;
    httpd_queue_work(g_server, do_async_send, a);
}
```

#### 5. При разрыве WebSocket-соединения

Реализовать через `esp_event` или polling `httpd_ws_get_fd_info()`:
при исчезновении viewer fd — установить `g_viewer_fd = -1`, вызвать `g_cb_peer_disconnected`.

#### 6. signaling_send_offer / signaling_send_ice

```c
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
```

### Проверка

`idf.py build` — без ошибок. Функциональный тест — в PROMPT-06.
```

---

## PROMPT-03 — WebRTC стек через libpeer

```
Реализуй модуль `webrtc_streamer.c` / `webrtc_streamer.h` для ESP32-P4 FPV стримера.

### Контекст

Используй библиотеку `libpeer` (espressif/libpeer в IDF Component Manager).
Документация: https://github.com/sepfy/libpeer

Протокол сигналинга реализован в `signaling.c` (уже готов).
Браузерный аналог: `public/js/webrtc-client.js`.

Viewer (Quest 2) ожидает:
- Codec: H.264 (Baseline или Main Profile)
- DataChannel: канал "fpv" (ordered, reliable) — создаётся стримером (ESP32)

### ICE в AP-режиме

ESP32 работает как точка доступа (AP), интернет недоступен.
STUN-сервер (`stun.l.google.com`) не нужен: ESP32 и Quest 2 находятся
в одной подсети (192.168.4.0/24) и ICE устанавливается напрямую через
host candidates. Настраивать ICE servers НЕ нужно:

```c
PeerConfiguration config = {
    .ice_servers   = {},
    .n_ice_servers = 0,
};
```

### Требования к webrtc_streamer.h

```c
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef void (*webrtc_on_connected_t)(void);
typedef void (*webrtc_on_disconnected_t)(void);
typedef void (*webrtc_on_datachannel_msg_t)(const char *msg, size_t len);

void webrtc_set_callbacks(
    webrtc_on_connected_t         on_connected,
    webrtc_on_disconnected_t      on_disconnected,
    webrtc_on_datachannel_msg_t   on_datachannel_msg
);

void webrtc_init(void);

// Вызывается из on_viewer_ready:
void webrtc_create_offer(void);

// Вызывается из on_answer / on_ice:
void webrtc_set_answer(const char *sdp);
void webrtc_add_ice_candidate(const char *candidate,
                              int mline_index,
                              const char *sdp_mid);

// Передача H.264 NAL-unit (из camera.c)
void webrtc_push_video_frame(const uint8_t *data, size_t len, uint64_t pts_ms);

// Отправка через DataChannel
void webrtc_datachannel_send(const char *msg);

// Сброс при отключении viewer
void webrtc_reset(void);
```

### Требования к webrtc_streamer.c

1. **webrtc_init**: создать PeerConnection без ICE-серверов (AP-режим),
   добавить H.264 видео-трек (`CODEC_H264`), создать DataChannel "fpv".
   Установить колбэки:
   - `on_ice_candidate` → `signaling_send_ice(...)`
   - `on_connection_state_change` → CONNECTED → `on_connected`; FAILED/CLOSED → `on_disconnected`
   - `on_datachannel_open` → уведомить datachannel.c
   - `on_datachannel_message` → передать в `on_datachannel_msg`

2. **webrtc_create_offer**: `peer_connection_create_offer()` → в колбэке вызвать
   `signaling_send_offer(sdp)`.

3. **webrtc_push_video_frame**: `peer_connection_send_video()`. Не блокировать.

4. **webrtc_reset**: `peer_connection_destroy()` → `webrtc_init()` заново.
   Вызывать при каждом новом viewer (on_viewer_ready).

### Проверка

`idf.py build` — без ошибок. Функциональный тест — в PROMPT-06.
```

---

## PROMPT-04 — Камера OV5647 + аппаратный H.264 энкодер

```
Реализуй модуль `camera.c` / `camera.h` для ESP32-P4-Function-EV-Board с OV5647 камерой.

### Контекст

Плата: ESP32-P4-Function-EV-Board (официальная отладочная плата Espressif).
Камера: OV5647 (Raspberry Pi Camera Module v1/v2), подключена через MIPI CSI.
IDF компоненты: `esp_camera`, `esp_h264`.

Целевые параметры видео: 1280×720, 30 FPS, H.264.

Сначала прочитай актуальную документацию компонентов:
- `idf_component.yml` в репозитории esp_camera на GitHub (espressif/esp32-camera)
- `idf_component.yml` в репозитории esp_h264 на GitHub (espressif/esp-h264)

Обрати внимание: API esp32-camera для ESP32-P4 с MIPI CSI отличается от DVP
(параллельного) интерфейса на ESP32/ESP32-S3.

### Требования к camera.h

```c
#pragma once
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
```

### Требования к camera.c

1. **camera_init** — инициализация OV5647 через MIPI CSI:
   ```c
   camera_config_t config = {
       .pin_pwdn     = -1,
       .pin_reset    = -1,
       .pin_xclk     = -1,        // MIPI CSI — XCLK не нужен
       .xclk_freq_hz = 24000000,
       .pixel_format = PIXFORMAT_YUV420,
       .frame_size   = FRAMESIZE_HD,      // 1280x720
       .fb_count     = 3,
       .fb_location  = CAMERA_FB_IN_PSRAM,
       .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
   };
   esp_camera_init(&config);
   ```
   Запустить FreeRTOS task `camera_task` (stack 8192, priority 5, core 1).

2. **camera_task** — захват + H.264 кодирование:
   ```c
   camera_fb_t *fb = esp_camera_fb_get();
   uint64_t capture_ts = esp_timer_get_time() / 1000;

   uint64_t t0 = esp_timer_get_time();
   // кодировать fb->buf (YUV420) → h264_buf через esp_h264_enc_hw_new()
   uint32_t encode_ms = (esp_timer_get_time() - t0) / 1000;

   frame_cb(h264_buf, h264_len, capture_ts, encode_ms);
   esp_camera_fb_return(fb);
   ```

3. **H.264 энкодер** (`esp_h264`):
   - `esp_h264_enc_hw_new()` (аппаратный VEU ESP32-P4)
   - Битрейт: 2 Мбит/с, GOP: 30 (I-frame каждые 30 кадров)
   - SPS+PPS отправлять при каждом новом WebRTC-соединении

4. Логировать первый закодированный кадр: размер, encode_ms.

### Проверка

`idf.py build` — без ошибок. Функциональный тест — в PROMPT-06.
```

---

## PROMPT-05 — DataChannel: ping/pong и ts-сообщения

```
Реализуй модуль `datachannel.c` / `datachannel.h` для ESP32-P4 FPV стримера.

### Контекст

Протокол DataChannel определён в `public/js/datachannel.js` (прочитай его).
Viewer (Quest 2 нативное приложение) реализует:
`fpv-native-quest/app/src/main/java/com/fpv/quest/FPVDataChannel.kt` (прочитай).

DataChannel "fpv" создаётся стримером (ESP32).

### Протокол

Входящие от viewer:
- `{"type":"ping","id":N,"t0":ms}` → немедленно ответить:
  `{"type":"pong","id":N,"t0":ms,"t1":<esp_timer_get_time()/1000>}`

Исходящие (каждую секунду через FreeRTOS timer):
- `{"type":"ts","capture":<ms>,"encode":<last_encode_ms>}`
  - `capture` = `esp_timer_get_time() / 1000`
  - `encode` = длительность H.264 кодирования последнего кадра

### Требования к datachannel.h

```c
#pragma once
#include <stdint.h>

void datachannel_on_open(void);
void datachannel_on_message(const char *json_str, size_t len);
void datachannel_on_close(void);

// Обновить encode_ms из camera_task (thread-safe)
void datachannel_update_encode_ms(uint32_t encode_ms);
```

### Требования к datachannel.c

1. **datachannel_on_open**: запустить повторяющийся FreeRTOS timer 1000 мс,
   колбэк отправляет ts-сообщение через `webrtc_datachannel_send()`.

2. **datachannel_on_message**: разбирать JSON cJSON:
   - `type == "ping"` → считать `id`, `t0`, отправить pong.

3. **datachannel_on_close**: остановить и удалить timer.

4. **datachannel_update_encode_ms**: сохранить в `volatile uint32_t g_encode_ms`
   или через `xQueueOverwrite` для защиты от гонки между camera_task и timer.

5. Логировать ts-отправки (DEBUG) и ping/pong (DEBUG).

### Проверка

`idf.py build` — без ошибок.
```

---

## PROMPT-06 — Интеграция, сборка, прошивка, тест

```
Заверши интеграцию всех модулей в `main.c`, собери, прошей и проверь сквозной тест.

### Контекст

Все модули готовы:
- signaling.c  — WiFi AP + HTTP + WebSocket сервер
- webrtc_streamer.c — WebRTC peer connection + DataChannel
- camera.c     — OV5647 + H.264 encoder
- datachannel.c — ping/pong + ts messages

Ноутбук и npm start НЕ нужны. ESP32 сама является сервером.

### Реализация main.c

```c
void app_main(void)
{
    // 1. NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // 2. Netif + event loop (нужны для WiFi)
    esp_netif_init();
    esp_event_loop_create_default();

    // 3. WebRTC (до signaling — будет вызван из on_viewer_ready)
    webrtc_set_callbacks(on_webrtc_connected,
                         on_webrtc_disconnected,
                         datachannel_on_message);
    webrtc_init();

    // 4. Camera (WebRTC уже готов принимать фреймы)
    camera_init(on_camera_frame);

    // 5. Signaling: поднять WiFi AP + HTTP-сервер + WS-сервер
    signaling_set_callbacks(on_viewer_ready,
                            webrtc_set_answer,
                            webrtc_add_ice_candidate,
                            on_peer_disconnected);
    signaling_start(SERVER_PORT);

    // Всё дальнейшее — через колбэки и таймеры
    vTaskDelay(portMAX_DELAY);
}
```

Реализуй статические колбэки:

```c
static void on_viewer_ready(void) {
    ESP_LOGI(TAG, "Viewer connected, creating offer...");
    webrtc_reset();
    webrtc_create_offer();
}

static void on_webrtc_connected(void) {
    ESP_LOGI(TAG, "WebRTC connected — streaming");
    datachannel_on_open();
}

static void on_webrtc_disconnected(void) {
    ESP_LOGI(TAG, "WebRTC disconnected");
    datachannel_on_close();
}

static void on_peer_disconnected(void) {
    ESP_LOGI(TAG, "Viewer disconnected, resetting WebRTC");
    datachannel_on_close();
    webrtc_reset();
}

static void on_camera_frame(const uint8_t *data, size_t len,
                            uint64_t pts_ms, uint32_t encode_ms) {
    datachannel_update_encode_ms(encode_ms);
    webrtc_push_video_frame(data, len, pts_ms);
}
```

### Сборка + прошивка SPIFFS

```bash
cd esp32-p4-streamer
idf.py set-target esp32p4
idf.py build

# Прошить firmware + SPIFFS-раздел с веб-файлами
idf.py -p /dev/cu.usbmodem* flash monitor
```

> `idf.py flash` автоматически прошивает и SPIFFS-образ (т.к. в CMakeLists.txt
> указан `FLASH_IN_PROJECT`). Убедись, что папка `public/` содержит `index.html`.

### Ожидаемый вывод монитора

```
I (xxx) main: FPV ESP32-P4 Streamer starting...
I (xxx) signaling: SPIFFS mounted at /www
I (xxx) signaling: WiFi AP started: SSID=FPV-Drone, IP=192.168.4.1, port=8080
I (xxx) camera: OV5647 initialized 1280x720@30fps
I (xxx) camera: First H.264 frame: 28470 bytes, encode=9ms
... (ожидание подключения Quest 2 к WiFi)
I (xxx) signaling: Viewer WebSocket connected, fd=54
I (xxx) signaling: Received role=viewer
I (xxx) main: Viewer connected, creating offer...
I (xxx) webrtc: Creating offer...
I (xxx) signaling: Sent offer (1342 bytes)
I (xxx) signaling: Received answer
I (xxx) webrtc: ICE gathering (host candidates only, no STUN)
I (xxx) webrtc: ICE connected via 192.168.4.1:XXXXX ↔ 192.168.4.2:XXXXX
I (xxx) main: WebRTC connected — streaming
I (xxx) datachannel: Channel open
I (xxx) datachannel: Sent ts: capture=... encode=9
```

### Сквозной тест

1. Подать питание на ESP32-P4 (USB-C от блока питания или ноутбука — ноутбук не нужен)
2. На Quest 2: WiFi → подключиться к **"FPV-Drone"**

**Нативное приложение (рекомендуется):**
- Запустить FPV Quest
- Кнопка Y → VR-панель → ввести IP `192.168.4.1`, порт остаётся 8080
- WS-адрес: `ws://192.168.4.1:8080/ws`
- Нажать A → Connect

**Браузер (для теста):**
- Oculus Browser → `http://192.168.4.1/`
- JS подключится к `ws://192.168.4.1:8080/` (root) — работает,
  т.к. WS-хэндлер зарегистрирован на обоих путях (`/` и `/ws`)

3. Через 3–5 секунд: видео с OV5647 в VR
4. В Quest HUD через ~5 с: E2E < 50 мс, encode > 0 мс ✓
5. Ctrl+] в мониторе — выход
```

---

## Справочник: сигналинг-протокол (AP-режим)

ESP32 — сервер. Нет промежуточного Node.js relay.

| Сообщение | Направление | JSON |
|-----------|-------------|------|
| Регистрация | Quest → ESP32 | `{"type":"role","role":"viewer"}` |
| Offer | ESP32 → Quest | `{"type":"offer","sdp":"v=0\r\n..."}` |
| Answer | Quest → ESP32 | `{"type":"answer","sdp":"v=0\r\n..."}` |
| ICE от ESP32 | ESP32 → Quest | `{"type":"ice","candidate":{"candidate":"candidate:...","sdpMLineIndex":0,"sdpMid":"0"}}` |
| ICE от Quest | Quest → ESP32 | `{"type":"ice","candidate":{"candidate":"...","sdpMLineIndex":0,"sdpMid":"0"}}` |

## Справочник: DataChannel-протокол

| Сообщение | Направление | JSON |
|-----------|-------------|------|
| Ping (clock sync) | Quest → ESP32 | `{"type":"ping","id":1,"t0":1234567890}` |
| Pong | ESP32 → Quest | `{"type":"pong","id":1,"t0":1234567890,"t1":1234567895}` |
| Timestamp | ESP32 → Quest (1/с) | `{"type":"ts","capture":1234567890,"encode":9}` |
