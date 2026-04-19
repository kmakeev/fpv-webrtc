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
#define CAM_WIDTH           1280
#define CAM_HEIGHT          720
#define CAM_FPS             30
