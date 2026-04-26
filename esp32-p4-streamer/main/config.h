#pragma once

// WiFi AP — ESP32-P4 поднимает точку доступа (192.168.4.1)
// C6 co-processor прошит esp-hosted-mcu v2.12.3 → AP-режим работает
#define WIFI_AP_SSID        "FPV-Drone"
#define WIFI_AP_PASS        "fpvdrone1"  // мин. 8 символов для WPA2

// HTTP + WebSocket сервер (запускается на ESP32)
// IP назначается роутером — смотри в логах: "sta ip: X.X.X.X"
#define SERVER_PORT         8080
#define WS_PATH             "/ws"
#define SPIFFS_BASE_PATH    "/www"

// Параметры камеры
// 800×640 RAW8 — default: encode ~25ms, stable 30fps, safe 6-buffer ring.
// Switch at runtime via DataChannel { type:'resolution', w:1280, h:960 }.
#define CAM_WIDTH           800
#define CAM_HEIGHT          640
#define CAM_FPS             30
