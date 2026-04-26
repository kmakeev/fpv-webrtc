# FPV WebRTC — предложения по улучшению

Приоритеты: **P1** — критично для надёжности в реальных условиях, **P2** — значимое улучшение UX/производительности, **P3** — полезные расширения функционала.

---

## P1 — Надёжность и стабильность

### 1. Адаптивный битрейт на ESP32 (ABR) ✅ реализовано

**Проблема.** Энкодер всегда работает на фиксированном битрейте (~4 Мбит/с). Quest отправляет PLI/FIR при потерях, ESP32 реагирует IDR-фреймом, но битрейт не снижается — потери продолжаются.

**Решение.** Использовать RTCP Receiver Report (RR), который libpeer уже получает: поле `fraction_lost` напрямую сигнализирует о потерях. Логика:
- `fraction_lost > 20/255` (~8%) → снизить битрейт на 20%
- `fraction_lost == 0` несколько секунд подряд → поднять битрейт на 10%
- Границы: [500 кбит/с, 8 Мбит/с]

Обновлять через `esp_h264_enc_set_bitrate()` без пересоздания энкодера.

**Файлы:** `webrtc_streamer.c` (callback `on_receiver_packet_loss_cb`), `camera.c` (`camera_set_bitrate()`, `camera_get_bitrate()`), `camera.h`.

---

### 2. Переподключение Quest при потере сигнала ✅ реализовано

**Проблема.** Если ESP32 перезагрузилась или WiFi AP пропал — нативное приложение и браузерный клиент зависают на чёрном экране. Нет автоматического reconnect с exponential backoff.

**Решение.**
- **Android** (`SignalingClient.kt`): при `onFailure` / `onClosing` / `peer_disconnected` запускать reconnect с backoff [1с, 2с, 4с, 8с, 30с], остановка по `close()`. Callback `onReconnecting` показывает таймер в UI/VR HUD.
- **Браузер** (`webrtc-client.js`): заменён фиксированный delay 3с на exponential backoff [1с…30с] с reset при успешном `onopen`.
- **ESP32** (`signaling.c`): при обрыве WiFi — `esp_wifi_connect()` в цикле (уже реализовано для station mode, для AP не нужно).

**Файлы:** `SignalingClient.kt`, `webrtc-client.js`, `MainActivity.kt`.

---

### 3. Детект зависания видео и запрос IDR ✅ реализовано

**Проблема.** Если RTP-поток идёт, но содержит ошибки (битые P-фреймы после потери), декодер Quest застывает на последнем валидном кадре. Сейчас IDR-retry — только на старте соединения.

**Решение.**
- В `stats.js`: если `framesDecoded` не растёт ≥3 секунд при `connectionState == 'connected'` — отправить `FPVDataChannel.send({ type: 'pli' })`.
- В `WebRTCEngine.kt`: freeze watcher coroutine следит за `EglVideoSink.frameCount`; при заморозке ≥3 с вызывает `dataChannel.sendVideoFreezeRequest()`.
- ESP32 (`datachannel.c`): при получении `pli` вызвать `camera_request_idr()` немедленно, минуя rate-limit 1.5 с (rate-limit сохранён только для сетевых PLI/FIR).

**Файлы:** `stats.js`, `FPVDataChannel.kt`, `WebRTCEngine.kt`, `datachannel.c`.

---

### 4. NVS-конфигурация WiFi на ESP32

**Проблема.** SSID и пароль хардкожены в `config.h`. Смена сети требует перекомпиляции и перепрошивки.

**Решение.** При первом старте (NVS пустой) поднять временную AP `FPV-Setup` (без пароля), выдать простую веб-страницу (1 HTML-файл в SPIFFS) с формой: SSID / Password / AP или Station mode. Сохранить в NVS. После сохранения — рестарт в штатном режиме.

Альтернатива для MVP: парсить SSID/PASS из Serial при первом запуске и сохранять в NVS — без веб-формы.

**Файлы:** `config.h`, `main.c`, новый `wifi_config.c/h`.

---

## P2 — Производительность и UX

### 5. Разрешение 800×640 как default с ручным переключением ✅ реализовано

**Проблема.** Сейчас encode throughput при 1280×960 RAW10 составляет ~12fps (encode ~98ms), что при 30fps сенсора создаёт нарастающую очередь. 800×640 RAW8 даёт encode ~25ms и стабильные 30fps.

**Решение.** Переведён `config.h` на 800×640 как default. Добавлено переключение через DataChannel-сообщение `{ type: 'resolution', w: 1280, h: 960 }` — `camera_set_resolution()` устанавливает флаг `g_res_pending`, camera_task (core 1) вызывает `_camera_reconfigure()` после фрейма (полный teardown/reinit ISP + CSI + H.264 encoder).

**Файлы:** `config.h`, `camera.c` (`camera_set_resolution()`, `_camera_reconfigure()`), `camera.h`, `datachannel.c`.

---

### 6. Drift-коррекция синхронизации часов

**Проблема.** Часы ESP32 (`esp_timer`) дрейфуют ±20-50 мс/час. Текущая синхронизация выполняется один раз при открытии DataChannel. E2E-задержка постепенно накапливает ошибку.

**Решение.** Повторять синхронизацию каждые 5 минут (не 5 round-trips, а 3 — для меньшей нагрузки). Обновлять `clockOffset` инкрементально (exponential moving average, α=0.3), чтобы не было скачков в UI.

**Файлы:** `datachannel.js`, `FPVDataChannel.kt`, `datachannel.c`.

---

### 7. Статистика потерь пакетов и FEC-индикатор

**Проблема.** Сейчас stats показывают RTT, Decode, Jitter. Нет информации о потерях пакетов (%) — главном индикаторе качества FPV-канала.

**Решение.** В `stats.js` из `inbound-rtp` брать `packetsLost` и `packetsReceived`, считать `lossRate = delta(packetsLost) / delta(packetsReceived+packetsLost)`. Показывать в VR HUD третьей строкой. Цветовой код: зелёный <1%, жёлтый <5%, красный ≥5%.

**Файлы:** `stats.js`, `xr_renderer.cpp` (цвет текста HUD).

---

### 8. Gimbal-управление через IMU Quest (передача ориентации головы)

**Проблема.** `type: 'head'` в DataChannel зарезервирован, но не реализован. Для FPV-дрона управление камерой по наклону головы — ключевой feature.

**Решение.**
- **Quest** (`FPVDataChannel.kt`): каждые 50ms отправлять кватернион из `XrView.pose` — не из хедсет-контроллера, а из view-матрицы левого глаза. Throttle: отправлять только при изменении >0.5°.
- **ESP32** (`datachannel.c`): принимать `{ type: 'head', x, y, z, w }`, конвертировать в углы Эйлера (pitch/yaw), выдавать PWM-сигнал на сервопривод через MCPWM или LEDC. GPIO для сервы — через `config.h`.

Это превращает FPV-систему из «смотровой» в управляемую — ключевое отличие от просто VR-трансляции.

**Файлы:** `FPVDataChannel.kt`, `XrRenderThread.kt`, `datachannel.c`, `config.h`, новый `gimbal.c/h`.

---

### 9. Запись видео на Quest (локальный DVR)

**Проблема.** Нет возможности записать полёт.

**Решение.** В `WebRTCEngine.kt`: при получении `MediaStreamTrack` создать `MediaRecorder` с `video/webm;codecs=h264` (Quest поддерживает). Кнопка на VR-панели (кнопка X левого контроллера) старт/стоп. Файлы сохраняются в `/sdcard/Movies/FPV/`. Отдельный thread, не блокирует рендер.

Запись прямо в H.264-потоке без перекодирования — нулевые накладные расходы.

**Файлы:** `WebRTCEngine.kt`, `MainActivity.kt`, `XrRenderThread.kt` (кнопка X).

---

### 10. OSD-телеметрия в VR HUD

**Проблема.** HUD показывает только сетевые метрики. Для FPV важна телеметрия борта: напряжение батареи, RSSI, высота, скорость.

**Решение.** Расширить DataChannel-протокол: ESP32 раз в секунду отправляет `{ type: 'telemetry', batt_mv: 11800, rssi: -65 }`. На Quest — отдельная строка HUD. Данные батареи ESP32 может читать через ADC (если подключена цепь делителя), RSSI — из `esp_wifi_sta_get_ap_info()` (в AP-режиме — `0`, в station-режиме — реально).

Для более богатой телеметрии — MAVLink через UART от полётного контроллера (отдельный UART на ESP32-P4 доступен) и парсинг `HEARTBEAT` / `SYS_STATUS`.

**Файлы:** `datachannel.c`, `FPVDataChannel.kt`, `xr_renderer.cpp`.

---

## P3 — Расширения функционала

### 11. Поддержка нескольких зрителей (multicast через server.js)

**Проблема.** Сейчас сервер держит ровно одного streamer и одного viewer. Несколько людей не могут смотреть одновременно.

**Решение.** В `server.js`: viewer-роль изменить на список (`viewers = new Map(id → ws)`). При появлении нового viewer — отправить ему буферизованный offer от streamer'а. Ответ каждого viewer направлять только его offer'у. ICE-кандидаты — индивидуально каждому. Ограничение: не более 3 viewer'ов (сетевая нагрузка).

**Файлы:** `server.js`.

---

### 12. Сохранение конфигурации VR-панели в нативном приложении

**Проблема.** Позиция и размер видео-квада сбрасываются при каждом перезапуске. Пользователь каждый раз заново настраивает.

**Решение.** Сериализовать `PanelTransform` в `SharedPreferences` при каждом изменении (drag/scale). Восстанавливать в `xrApp_init()`. Кнопка «Reset» на панели — сброс к default.

**Файлы:** `XrRenderThread.kt`, `xr_renderer.cpp`, новый JNI callback.

---

### 13. Автодетект ESP32 в сети (mDNS / UDP broadcast)

**Проблема.** IP-адрес ESP32 (`192.168.4.1` в AP-режиме) фиксирован, но в station-режиме — динамический DHCP. Нативное приложение требует ввода IP вручную.

**Решение.**
- **ESP32**: зарегистрировать mDNS-запись `fpv-drone.local` через `esp_mdns` компонент (входит в IDF). Одна строка кода.
- **Android**: использовать `NsdManager` для поиска `_http._tcp` с именем `fpv-drone`. При обнаружении — автозаполнить поле URL.

В AP-режиме mDNS не нужен — адрес всегда 192.168.4.1.

**Файлы:** `main.c`, `MainActivity.kt`.

---

### 14. Watchdog и автоперезапуск на ESP32

**Проблема.** При зависании camera_task или peer_loop_task (например, deadlock на spinlock) — ESP32 зависает. Единственный выход — физический сброс.

**Решение.** Использовать `esp_task_wdt_add()` для camera_task и peer_loop_task. Каждые N секунд — `esp_task_wdt_reset()`. При срабатывании WDT — автоперезапуск (уже настроен в IDF: `CONFIG_ESP_TASK_WDT_PANIC=y`). Добавить `CONFIG_ESP_TASK_WDT_TIMEOUT_S=10` в `sdkconfig.defaults`.

**Файлы:** `camera.c`, `webrtc_streamer.c`, `sdkconfig.defaults`.

---

### 15. Репортинг E2E breakdown по компонентам

**Проблема.** Сейчас E2E задержка — одно число. Непонятно, где узкое место: WiFi, jitter-буфер, декодер, или рендер.

**Решение.** В `stats.js` разбить E2E на компоненты и показать timeline-бар в VR HUD:
```
[WiFi ██ 8ms][Jitter ████ 20ms][Decode ██████ 38ms] = 66ms
```
Данные уже есть: `RTT/2` (WiFi), `jitterBufferDelay` (Jitter), `totalDecodeTime` (Decode). E2E из DataChannel служит как cross-check.

**Файлы:** `stats.js`, `xr_renderer.cpp` (рендер bar-chart).

---

## Что намеренно не включено

- **H.265 / AV1** — libpeer и Quest браузер не поддерживают; нативное приложение теоретически может, но потребует полной замены WebRTC-стека.
- **TURN-сервер** — проект принципиально LAN-ориентирован; TURN ломает этот дизайн.
- **Аутентификация / mTLS** — overkill для персонального FPV-устройства в домашней сети.
- **Сжатие аудио** — микрофон на OV5647 отсутствует; аудио-стрим требует отдельного I2S-микрофона и значительного усложнения пайплайна.
