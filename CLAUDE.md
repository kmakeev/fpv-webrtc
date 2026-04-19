# CLAUDE.md

Этот файл содержит инструкции для Claude Code (claude.ai/code) при работе с данным репозиторием.

## Обзор проекта

Приложение FPV-стриминга на базе WebRTC, передающее видео с камеры ESP32-P4 (или веб-камеры ноутбука) на гарнитуру Oculus Quest 2 для стереоскопического VR-просмотра.

## Команды

```bash
npm install          # Установить зависимости (пакет ws)
npm start            # Запустить HTTP-сервер на порту 8080
npm run dev          # Запустить с --watch (авто-перезагрузка)
TLS=1 npm start      # Запустить HTTPS-сервер (обязательно для WebXR на Quest 2)
./gen-certs.sh       # Сгенерировать самоподписанные TLS-сертификаты (server/certs/key.pem + cert.pem)
```

Переменные окружения: `PORT` (по умолчанию 8080), `HOST` (по умолчанию 0.0.0.0), `TLS=1` — включает HTTPS.

Тесты и линтер не настроены.

## Команды Docker

```bash
docker compose --profile http up                       # HTTP-режим (локальная разработка)
./gen-certs.sh && docker compose --profile tls up     # HTTPS/TLS-режим (обязательно для Quest 2)
PORT=9090 docker compose --profile http up             # Изменить порт хоста
```

Сертификаты генерируются на хосте командой `./gen-certs.sh` — они содержат IP хоста, который Docker не знает. Затем монтируются в контейнер как volume `./server/certs:/app/server/certs:ro`.

## Архитектура

Двухпировая модель WebRTC с центральным сигналинг-сервером:

**server/server.js** — Node.js HTTP(S) + WebSocket-сервер. Раздаёт `public/` как статику и ретранслирует сигналинг-сообщения между пирами "streamer" и "viewer". ICE-кандидаты буферизуются до подключения второго пира. Типы WS-сообщений: `role`, `offer`, `answer`, `ice`, `viewer_ready`, `peer_disconnected`.

**public/streamer.html** — Автономная страница стримера. Захватывает веб-камеру при 1280×720@30fps, создаёт WebRTC offer и отправляет вьюверу. Для реального FPV вместо неё используется ESP32-P4.

**public/index.html** — Страница вьювера для Quest 2. Загружает пять JS-модулей:

- **datachannel.js** — Общий модуль WebRTC DataChannel для стримера и вьювера. Реализует NTP-подобную синхронизацию часов (5 round-trips, медиана смещения, старт через 3 с после открытия канала — чтобы jitter-буфер успел устояться). Стример создаёт канал (`FPVDataChannel.create(pc)`), вьювер принимает (`FPVDataChannel.accept(pc)`). Активные типы сообщений: `ping`/`pong` (sync), `ts` (временны́е метки захвата). Тип `head` (поза головы) зарезервирован для будущего управления через контроллеры.
- **webrtc-client.js** — Управляет RTCPeerConnection. Регистрируется как "viewer", получает offer, создаёт answer, обрабатывает ICE. Переставляет SDP для приоритета H.264 (аппаратное ускорение на Quest 2). Принимает DataChannel и запускает синхронизацию часов с задержкой 3 с.
- **webxr-renderer.js** — Стерео-рендеринг через WebGL2 + WebXR. Отрисовывает входящее видео на полноэкранный quad дважды (левый/правый глаз) со смещением viewport. IPD: 0.064 м, FOV: 90°, виртуальное расстояние: 1.5 м. Экспортирует кватернион головы через `onHeadPose` (в данный момент не используется — управление камерой будет через контроллеры).
- **stats.js** — Опрашивает `RTCPeerConnection.getStats()` каждую секунду для отображения задержки, FPS, разрешения и кодека в UI. Принимает E2E-метрики через `FPVStats.updateE2E()`. Примечание: `requestVideoFrameCallback.processingDuration` на Android/Quest возвращает ту же async MediaCodec pipeline latency что и `totalDecodeTime` — rVFC не используется (добавляет нагрузку без улучшения точности).
- **main.js** — Оркестрирует все модули. Управляет переходами состояний UI (connecting → waiting → streaming). Передаёт входящий MediaStream как на плоское видео, так и в WebXR-рендерер. Вычисляет E2E-задержку из временны́х меток DataChannel с учётом смещения часов.

## Статистика

Модуль `stats.js` опрашивает `RTCPeerConnection.getStats()` каждую секунду и отображает:

| Метрика | Источник | Примечание |
|---------|----------|------------|
| Сеть | RTT/2 из `candidate-pair.currentRoundTripTime` | Приближение, предполагает симметричный канал |
| Декод | `totalDecodeTime / framesDecoded` за интервал | На Android/Quest ~38ms (MediaCodec pipeline), на desktop ~1ms |
| Буфер | `jitterBufferDelay / jitterBufferEmittedCount` | Надёжная метрика |
| Итого | Сеть + Декод + Буфер | Завышен на Quest из-за Декод |
| E2E | DataChannel: `viewer_now − capture + clockOffset` | SCTP latency ≈ чистая сеть, не включает буфер и декод |
| Энкод | `encode − capture` из DataChannel | Для вебкамеры ≈ 0; для ESP32-P4 — реальное время H.264 |

Статистика выводится в трёх местах:
- **Полоска** под кнопками — E2E когда доступна, иначе Net RTT/2
- **Flat overlay** (`#flat-stats`) — поверх видео в режиме "Смотреть на экране"; строка E2E показывается динамически
- **WebGL HUD** — квад в world-space поверх видео в VR-режиме (рендерится через те же XR view/projection матрицы, обновляется внутри XR-фрейма через `_uploadStatsTexture`); E2E добавляется во вторую строку

## DataChannel

Канал `'fpv'` (ordered, reliable) создаётся стримером и принимается вьювером автоматически при WebRTC-соединении.

**Алгоритм синхронизации часов:**
1. Вьювер отправляет 5 `ping`-сообщений с меткой `t0`
2. Стример немедленно отвечает `pong` с `t0` и `t1 = Date.now()`
3. Вьювер вычисляет `offset = t1 - t0 - RTT/2` для каждого round-trip
4. `clockOffset = median(5 измерений)` — точность ±1–2 мс на LAN

**Расчёт E2E задержки:**
```
e2eMs = Date.now()_viewer - captureTs_streamer + clockOffset
```

**Типы сообщений DataChannel:**

| Тип | Направление | Содержимое |
|-----|-------------|-----------|
| `ping` | viewer → streamer | `{ id, t0 }` |
| `pong` | streamer → viewer | `{ id, t0, t1 }` |
| `ts`   | streamer → viewer | `{ capture, encode }` — раз в секунду |
| `head` | viewer → streamer | `{ x, y, z, w }` — зарезервировано, не отправляется (управление через контроллеры) |

## Нативное приложение (fpv-native-quest/)

Android-приложение для Quest 2, которое заменит браузерный просмотрщик и снизит E2E-задержку с ~65–100 мс до ~15–35 мс.

### Структура проекта

```
fpv-native-quest/                    # Android Studio project root
├── settings.gradle / build.gradle   # Gradle 8.7, AGP 8.3, Kotlin 1.9
├── gradlew                          # Gradle wrapper (chmod +x уже выставлен)
├── local.properties.template        # Скопировать → local.properties, указать sdk.dir
└── app/
    ├── build.gradle                 # minSdk=29, targetSdk=32, arm64-v8a, NDK
    └── src/main/
        ├── AndroidManifest.xml      # INTERNET, VR headtracking, landscape
        ├── java/com/fpv/quest/
        │   ├── MainActivity.kt      # URL input + Connect; WebRTCEngine+Signaling init; lifecycle; XrRenderThread
        │   ├── SignalingClient.kt   # OkHttp WebSocket, протокол = server.js
        │   ├── WebRTCEngine.kt      # PeerConnectionFactory + HardwareVideoDecoderFactory + полный PeerConnection.Observer
        │   ├── EglVideoSink.kt      # TASK-004: VideoSink → извлекает TextureBuffer(OES) → JNI nativeUpdateVideoFrame
        │   ├── FPVDataChannel.kt    # NTP clock sync + E2E + bind(DataChannel); зеркало datachannel.js
        │   └── XrRenderThread.kt   # TASK-005: dedicated thread, shared EGL context, XR frame loop
        ├── cpp/
        │   ├── CMakeLists.txt       # libfpv-native.so, links EGL + GLESv3 + log + mediandk + openxr_loader (Prefab)
        │   ├── video_state.h        # TASK-005: non-JNI accessors for OES texture (videostate_getTexId/Matrix)
        │   ├── xr_renderer.cpp      # TASK-005: OpenXR 1.0 session + stereo rendering + samplerExternalOES shader
        │   └── video_decoder.cpp    # TASK-004: OES texture globals (g_videoTexId, g_stMatrix) + glFlush
        └── res/
            ├── layout/activity_main.xml         # SurfaceViewRenderer + overlay + status strip
            └── xml/network_security_config.xml  # ws:// cleartext для LAN
```

### Требования к окружению

| Компонент | Версия | Установка |
|-----------|--------|-----------|
| JDK | 17 | `brew install openjdk@17` |
| NDK | 27.x | Android Studio → SDK Manager → SDK Tools |
| CMake | 3.22.1 | Android Studio → SDK Manager → SDK Tools |
| Android API | 32 | Android Studio → SDK Manager → SDK Platforms |

Подробная инструкция: `docs/native-android-setup.md`.

### Gradle-команды

```bash
cd fpv-native-quest/

# Первый запуск: создать local.properties
cp local.properties.template local.properties
# Отредактировать sdk.dir → /Users/konstantin/Library/Android/sdk

# Установить JAVA_HOME если JDK 17 не в PATH (macOS Homebrew)
export JAVA_HOME=/opt/homebrew/opt/openjdk@17

# ОБЯЗАТЕЛЬНО: потянуть Meta OpenXR loader с подключённого Quest 2
# (Khronos loader из Maven не обнаруживает Meta runtime — XR_ERROR_RUNTIME_UNAVAILABLE)
adb pull /system/lib64/libopenxr_loader.so app/src/main/jniLibs/arm64-v8a/libopenxr_loader.so

# Сборка debug APK
./gradlew assembleDebug
# APK: app/build/outputs/apk/debug/app-debug.apk

# Сборка + установка на подключённый Quest 2
./gradlew installDebug

# Запуск установленного приложения через ADB
adb shell am start -n com.fpv.quest/.MainActivity

# Логи в реальном времени
adb logcat -s FPVQuest SignalingClient WebRTCEngine FPVDataChannel xr_renderer video_decoder

# Переустановка без удаления данных
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

### Зависимости (app/build.gradle)

| Библиотека | Версия | Назначение |
|------------|--------|-----------|
| `io.github.webrtc-sdk:android` | 125.6422.07 | Google pre-built libwebrtc: PeerConnectionFactory, HardwareVideoDecoderFactory, DataChannel |
| `com.squareup.okhttp3:okhttp` | 4.12.0 | WebSocket для сигналинга |
| `kotlinx-coroutines-android` | 1.8.0 | Async clock sync, WebSocket callbacks |
| `org.khronos.openxr:openxr_loader_for_android` | 1.1.49 | **Только хедеры** (`compileOnly`); Prefab AAR; CMake таргет `OpenXR::headers`. Сам .so — Meta loader из `jniLibs/` (см. ADB pull выше) |

### Соответствие JS ↔ Kotlin/C++ модулей

| JS (public/js/) | Kotlin / C++ (com/fpv/quest/ + cpp/) | Статус |
|-----------------|--------------------------------------|--------|
| `webrtc-client.js` | `WebRTCEngine.kt` + `SignalingClient.kt` | ✅ TASK-003 |
| `datachannel.js` | `FPVDataChannel.kt` | ✅ TASK-006 (bind + clock sync; E2E в status bar + VR HUD) |
| *(нет аналога)* | `EglVideoSink.kt` + `video_decoder.cpp` | ✅ TASK-004 |
| `webxr-renderer.js` | `xr_renderer.cpp` + `XrRenderThread.kt` | ✅ TASK-005 |
| `stats.js` | inline в `MainActivity.kt` + `xr_renderer.cpp` (stats HUD overlay) | ✅ TASK-006 |

## ESP32-P4 прошивка (esp32-p4-streamer/)

Автономный FPV-стример на базе ESP32-P4-Function-EV-Board с камерой OV5647. Заменяет `streamer.html` и Node.js-сервер: ESP32 сама является и стримером, и сигналинг-сервером.

**Архитектура:** ESP32 поднимает WiFi AP "FPV-Drone" (192.168.4.1), запускает HTTP+WebSocket сервер на порту 8080, раздаёт `public/` через SPIFFS и выступает WebRTC-стримером. Ноутбук нужен только для сборки и прошивки.

### Структура проекта

```
esp32-p4-streamer/
├── CMakeLists.txt          # top-level cmake; SPIFFS из ../public/
├── sdkconfig.defaults      # ESP32-P4: SPIRAM OCT, WiFi AMPDU, HTTPD WS, FreeRTOS 1000Hz
├── partitions.csv          # NVS(24KB) + PHY(4KB) + factory(4MB) + OTA(4MB) + SPIFFS(2MB)
├── idf_component.yml       # esp_camera + esp_h264 + libpeer
└── main/
    ├── CMakeLists.txt      # idf_component_register
    ├── config.h            # константы: SSID/PASS/PORT/WS_PATH/CAM params
    ├── main.c              # app_main: NVS → netif → webrtc → camera → signaling
    ├── signaling.c/h       # WiFi AP + HTTP static + WebSocket сигналинг (PROMPT-02)
    ├── webrtc_streamer.c/h # libpeer PeerConnection + DataChannel (PROMPT-03)
    ├── camera.c/h          # OV5647 MIPI CSI + esp_h264 HW encoder (PROMPT-04)
    └── datachannel.c/h     # ping/pong clock sync + ts timestamps (PROMPT-05)
```

### Требования к окружению

| Компонент | Версия | Примечание |
|-----------|--------|-----------|
| ESP-IDF | **≥ 5.4** | 5.3 не поддерживает ESP32-P4 ревизию v1.x (production) |
| Таргет | esp32p4 | |

### Команды сборки

```bash
cd esp32-p4-streamer

# Первая сборка (загружает managed_components, ~3–5 мин)
idf.py set-target esp32p4
idf.py build

# Прошивка + SPIFFS (FLASH_IN_PROJECT — прошивается автоматически)
idf.py -p /dev/cu.usbmodem* flash monitor

# Только логи (без перепрошивки)
idf.py -p /dev/cu.usbmodem* monitor

# Ctrl+] — выход из монитора
```

### Ключевые настройки sdkconfig.defaults

| Ключ | Значение | Причина |
|------|----------|---------|
| `CONFIG_ESPTOOLPY_FLASHSIZE` | `"16MB"` | ESP32-P4-Function-EV-Board имеет 16MB flash |
| `CONFIG_PARTITION_TABLE_CUSTOM` | `y` | Кастомная таблица с SPIFFS(2MB) и OTA(4MB) |
| `CONFIG_MBEDTLS_SSL_PROTO_DTLS` | `y` | Нужен libpeer (DTLS для WebRTC) |
| `CONFIG_MBEDTLS_SSL_DTLS_SRTP` | `y` | Нужен libpeer (SRTP профили) |
| `CONFIG_SPIRAM_MODE_OCT` | `y` | ESP32-P4 PSRAM в Octal-режиме |
| `CONFIG_ESP_WIFI_REMOTE_ENABLED` | `y` | ESP32-P4 WiFi через `esp_wifi_remote` → ESP32-C6 coprocessor по SDIO |
| `CONFIG_ESP_HOSTED_ENABLED` | `y` | esp-hosted-mcu v2.12.3 host driver (соответствует прошивке C6) |
| `CONFIG_ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD` | `y` | Preset GPIO SDIO-пинов для ESP32-P4-Function-EV-Board (CLK=18, CMD=19, D0–D3=14–17, RST=54) |

### Прошивка ESP32-C6 (однократно, при смене платы)

C6 на ESP32-P4-Function-EV-Board идёт с factory firmware esp-hosted v0.0.6, несовместимой с IDF 5.4. Нужно обновить до v2.12.3 через OTA:

```bash
# 1. Собрать slave firmware для C6
cd ~/esp/esp-hosted-mcu/slave
idf.py set-target esp32c6 && idf.py build

# 2. Скопировать бинарник в OTA-пример
cp build/network_adapter.bin \
   ~/esp/esp-hosted-mcu/examples/host_performs_slave_ota/components/ota_littlefs/slave_fw_bin/

# 3. Прошить OTA-пример на P4 (он прошьёт C6 через SDIO)
cd ~/esp/esp-hosted-mcu/examples/host_performs_slave_ota
rm -f sdkconfig
idf.py set-target esp32p4 && idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
# Успех: "Slave firmware version: 2.12.3" + "Versions compatible - OTA not required"
```

### Исправления в managed_components

После скачивания компонентов (первый `idf.py build`) требуются однократные патчи:

```bash
# sepfy/libpeer использует cmake_minimum_required(VERSION 3.1) — несовместимо с CMake ≥ 3.28
sed -i '' 's/cmake_minimum_required(VERSION 3.1)/cmake_minimum_required(VERSION 3.5)/' \
    managed_components/sepfy__libpeer/CMakeLists.txt

# sepfy/srtp: GCC 14 делает unsigned int* vs uint32_t* hard error на RISC-V
# Добавить в конец managed_components/sepfy__srtp/CMakeLists.txt:
echo 'target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-incompatible-pointer-types)' \
    >> managed_components/sepfy__srtp/CMakeLists.txt
```

> Если `managed_components/` удалить и скачать заново — патчи нужно повторить.

### Статус реализации

| Файл | Назначение | Статус |
|------|-----------|--------|
| `main.c` | Инициализация, оркестрация | ✅ PROMPT-01 (skeleton) |
| `config.h` | Константы | ✅ PROMPT-01 |
| `signaling.c/h` | WiFi AP + HTTP + WebSocket | ✅ PROMPT-02 |
| `webrtc_streamer.c/h` | WebRTC (libpeer) | ✅ PROMPT-03 |
| `camera.c/h` | OV5647 + H.264 HW encoder | 🔲 PROMPT-04 |
| `datachannel.c/h` | DataChannel ping/pong + ts | 🔲 PROMPT-05 |

### Особенности AP-режима

- **WiFi архитектура**: ESP32-P4 (без встроенного WiFi) использует ESP32-C6 как WiFi-сопроцессор по SDIO. Стек: `esp_wifi_remote` (компонент) оборачивает стандартный `esp_wifi` API и транслирует вызовы через `esp_hosted` (компонент, host-side driver v2.12.3) → SDIO → C6 firmware (esp-hosted-mcu v2.12.3). C6 выполняет реальную WiFi-операцию и возвращает результат. Важно: встроенный IDF-драйвер `CONFIG_ESP_HOST_WIFI_ENABLED` несовместим с C6 firmware v2.12.3 — нужны именно компоненты `espressif/esp_hosted` + `espressif/esp_wifi_remote`. `esp_phy` для ESP32-P4 — пустой стаб.
- STUN-сервер не нужен: ESP32 и Quest 2 в одной подсети (192.168.4.0/24), ICE через host candidates.
- WebSocket регистрируется на двух путях: `/` (браузерный viewer) и `/ws` (нативное приложение Quest).
- `signaling_send_json` использует `httpd_queue_work` для thread-safe отправки из любого FreeRTOS-таска.
- `httpd_ws_get_fd_info(hd, fd)` возвращает `httpd_ws_client_info_t` напрямую (не через указатель); разрыв = `!= HTTPD_WS_CLIENT_WEBSOCKET`.
- OV5647 подключена через MIPI CSI (не DVP) — API `esp_camera` отличается от ESP32-S3.
- H.264 кодируется аппаратным VEU (`esp_h264_enc_hw_new`): битрейт 2 Мбит/с, GOP 30.
- `encode_ms` в DataChannel `ts`-сообщениях — реальное время кодирования (в отличие от `streamer.html`, где ≈ 0).

## Ключевые технические ограничения

- WebXR требует HTTPS (`TLS=1`), кроме случая `localhost`.
- TLS-сертификаты должны находиться по путям `server/certs/key.pem` и `server/certs/cert.pem` до запуска с `TLS=1`. При их отсутствии сервер выводит ошибку и завершается.
- Нативному приложению (`fpv-native-quest`) TLS не требуется — используй `ws://` с `npm start` (без TLS=1). Если нужен `wss://` (например, сервер уже запущен с TLS), `SignalingClient` принимает любой TLS-сертификат (trust-all TrustManager) — Quest 2 не поддерживает установку user CA-сертификатов через UI.
- Сервер выставляет заголовки безопасности `Cross-Origin-Embedder-Policy: require-corp` и `Cross-Origin-Opener-Policy: same-origin` (нужно для SharedArrayBuffer).
- STUN: `stun.l.google.com:19302`. TURN-сервер отсутствует — оба пира должны быть в одной LAN или иметь прямую связность.
- Смещение глаз в `webxr-renderer.js` захардкожено (0.03 м на глаз), не учитывает перспективу.
- DataChannel реализован (`datachannel.js`): синхронизация часов и E2E-задержка работают. Передача позы головы (`type:'head'`) отключена — управление камерой будет через контроллеры Quest.
- `syncClocks()` запускается через 3 с после открытия DataChannel — иначе 5 быстрых SCTP ping-pong раскачивают jitter-буфер с ~19ms до ~40ms.
- Стример (`streamer.html`) отправляет `type:'ts'` с `capture = encode = Date.now()` — энкод-задержка вебкамеры в браузере недоступна; для ESP32-P4 это поле будет содержать реальное время H.264-кодирования.
- Метка времени захвата (`capture`) — `Date.now()` в момент отправки `ts`-сообщения, не RTP-timestamp.
- `totalDecodeTime` на Android/Quest включает async MediaCodec pipeline latency (~38ms), а не только декодирование (~2ms). `requestVideoFrameCallback.processingDuration` возвращает ту же величину — rVFC отклонён (добавляет нагрузку без улучшения точности).
- OpenXR loader (`libopenxr_loader.so`) должен быть потянут с устройства: `adb pull /system/lib64/libopenxr_loader.so app/src/main/jniLibs/arm64-v8a/`. Khronos loader из Maven Central не обнаруживает Meta runtime на Quest 2 (`XR_ERROR_RUNTIME_UNAVAILABLE -51`). Хедеры берутся из Khronos Prefab AAR (`org.khronos.openxr:openxr_loader_for_android:1.1.49`, `compileOnly`); CMake таргет `OpenXR::headers`.
- OES-текстура из WebRTC (`g_videoTexId`) доступна в XR render thread через shared EGL context (`EglBase.create(eglBase.eglBaseContext)`). `video_decoder.cpp` вызывает `glFlush()` после каждого обновления текстуры для cross-context видимости на Adreno (Quest 2).
- `XrRenderThread` стартует в `onResume()` (немедленно, до подключения WebRTC) и останавливается в `onDestroy()`. НЕ останавливается в `onPause()`/`onStop()` — XR runtime управляет состоянием сессии через `XR_SESSION_STATE_*` события сам. `nativeRenderFrame()` блокируется внутри `xrWaitFrame()` (~13.9 мс при 72 Гц). При отсутствии видео рендерит чёрный кадр (g_videoTexId=0), видео появляется автоматически при получении фреймов через EglVideoSink.
- `com.oculus.intent.category.VR` обязателен в манифесте. Без него Quest уничтожает Activity (`onPause→onStop→onDestroy`) при переходе в VR-режим, посылая `XR_SESSION_STATE_STOPPING` сразу после `onPause`. С этой категорией Activity остаётся жива. Приложение авто-подключается к сохранённому URL при старте (`onResume`); первичная настройка URL — через оверлей (виден до захвата XR-фокуса) или ADB.
- `<meta-data android:name="com.oculus.vr.focusaware" android:value="true" />` обязателен — без него сессия не достигает `XR_SESSION_STATE_FOCUSED` и контроллерный ввод не доставляется.
- Шейдер использует `samplerExternalOES` + `GL_OES_EGL_image_external_essl3`. ST-матрица (3×3, row-major из Android) передаётся в GLSL с `glUniformMatrix3fv(..., GL_TRUE, ...)` (transpose=true). IPD и FOV берутся из `XrView` (реальные значения шлема), расстояние 1.5 м и ширина экрана 2.0 м совпадают с `webxr-renderer.js`.
- Stats HUD overlay: `XrRenderThread` держит `AtomicReference<LongArray?>` для thread-safe передачи E2E/encode метрик из IO-потока DataChannel в render thread. Перед каждым `nativeRenderFrame()` render thread проверяет pending-обновление: рисует текст через Android Canvas на Bitmap 256×64 px, загружает как `GL_TEXTURE_2D` через `GLUtils.texImage2D()`, сообщает texture ID в C++ через `nativeSetStatsTexture()` (`g_statsTexId` — `std::atomic<GLuint>`). В `xr_renderer.cpp` overlay рендерится как **world-space quad** (0, 0.95 м, −2.0 м) в LOCAL-пространстве — над видео-экраном — с alpha-blending. `nativeSetStatsTexture()` вызывается **каждый** раз после загрузки Bitmap, а не только при создании текстуры — иначе после `clearStats()` (g_statsTexId=0) повторное подключение обновляло бы содержимое текстуры, не регистрируя её в C++, и HUD оставался бы невидимым.
- Status overlay: при отсутствии видеопотока (`hasVideo = false`) рендерится отдельный world-space quad (0, 0, −1.5 м) с текстовым сообщением (512×80 px). Управляется через `showStatus(msg)` / `nativeSetStatusTexture()`. `teardown()` вызывает `nativeResetVideoState()` (сбрасывает `g_videoTexId = 0`) — иначе placeholder OES-текстура, созданная при старте, всегда ненулевая и `hasVideo` никогда не становится `false`.
- VR connection panel: нажатие кнопки Y на левом контроллере открывает/закрывает панель (512×160 px) в world-space на (0, 0, −1.0 м) — ближе видео (1.5 м), поэтому перекрывает его. Левый стик X (порог 0.80) перемещает курсор между октетами IP (0..3); стик Y (порог 0.80) меняет значение выбранного октета (0..255). Оба оси: немедленное срабатывание при первом пересечении порога + авто-повтор (X: 500 мс, Y: 350 мс); ось с большим отклонением подавляет другую (защита от диагонального дрейфа). Кнопка A подтверждает URL и вызывает `onConnectRequested` на main thread. Входной action set: `XrActionSet` + 3 `XrAction` (Y-button bool, A-button bool, left thumbstick vector2f); результаты читаются через `nativeGetLastInputState(float[4])` после каждого `nativeRenderFrame()`. `XrSyncActions` выполняется в `xrApp_renderFrame()` после `xrBeginFrame` (требование OpenXR spec).
- ICE trickle формат: Chrome сериализует `RTCIceCandidate` как вложенный JSON-объект `{ candidate: { candidate:"...", sdpMLineIndex:0, sdpMid:"0" } }`. `SignalingClient.kt` проверяет тип поля `candidate` — если JSONObject, извлекает поля из вложенного объекта; если строка — читает из плоского формата (для симметрии). `streamer.html` при получении плоского кандидата (от Quest) строит `RTCIceCandidateInit` явно; при объектном (от браузерного вьювера) — передаёт напрямую. Несоответствие форматов приводило к молчаливому дропу всех trickle-кандидатов → ICE не соединялся.
- Центрирование и reference space: используется `XR_REFERENCE_SPACE_TYPE_LOCAL` (Y=0 = уровень глаз пользователя в момент последнего recentre). При нажатии кнопки Oculus runtime генерирует `XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING`; последующие `xrLocateViews` автоматически возвращают позиции в обновлённом LOCAL-фрейме — контент возвращается по центру без каких-либо действий на стороне приложения. `XR_REFERENCE_SPACE_TYPE_STAGE` (origin на уровне пола) не используется: видео на Y=0 оказывалось бы на уровне пола.
- **TASK-007 QoS**: WebRTC field trials (в `WebRTCEngine.kt`): `SendSideBwe-WithOverhead/Enabled/` (BWE учитывает заголовки), `Video-MinPlayoutDelay/min_ms:0/` (jitter-буфер без минимальной задержки), `ReducedRtcpPollingInterval/Enabled/` (RTCP обновляется чаще → RTT-оценки точнее). WiFi High Performance Lock (`WIFI_MODE_FULL_HIGH_PERF`, требует разрешений `CHANGE_WIFI_STATE` + `WAKE_LOCK`) удерживается с момента `startConnection()` до `onStop()` — устраняет latency spikes от WiFi power-save (10–50 мс). CPU/GPU BOOST через `XR_EXT_performance_settings` (`xrPerfSettingsSetPerformanceLevelEXT`): вызывается после `xrCreateSession` для обоих доменов (`CPU_EXT`, `GPU_EXT`); расширение проверяется как опциональное (`xrEnumerateInstanceExtensionProperties`) перед включением в `xrCreateInstance`. Тепловые throttling-события логируются через `XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT`.
- **TASK-009 grab & scale**: видео-квад управляется структурой `g_panel` (`PanelTransform{XrPosef, width, height}`) вместо захардкоженных констант. Вся логика работает в C++ внутри `xrApp_renderFrame()`. Три новых `XrAction` в том же `XrActionSet "fpv"`: `gripAction` (right squeeze/click, boolean) → drag; `rightStickAction` (right thumbstick, vector2f) → scale Y-осью во время grip; `rightAimAction` (right aim pose) + `rightAimSpace` → позиция контроллера в LOCAL-пространстве. Drag: панель перемещается 1:1 с дельтой aim-позиции (`g_drag.prevAim`). Scale: right stick Y > 0.5 → width × 1.02, < −0.5 → width × 0.98, ограничение [0.5, 4.0] м, высота пересчитывается как 9/16 × width. Stats HUD следует за панелью: `statsY = panel.y + panel.height/2 + STATS_HALF_H + 0.07`, `statsDist = −panel.z + 0.5`. Status overlay и stats шейдер получили новый uniform `u_cx` (horizontal centre). Recentre (Oculus button) → `g_panel = PanelTransform{}` (сброс в (0, 0, −1.5 м), 2×1.125 м).
- **libpeer `onicecandidate` (ESP32-P4)**: callback `on_ice_candidate_cb(char *sdp, void *userdata)` вызывается libpeer с **полным SDP offer** (включая встроенные `a=candidate` host-кандидаты) — это не trickle ICE. Из callback вызывается `signaling_send_offer(sdp)`, а НЕ `signaling_send_ice()`. DataChannel "fpv" создаётся через `peer_connection_create_datachannel()` только в `on_state_change_cb` при `PEER_CONNECTION_COMPLETED` (не CONNECTED). `peer_connection_loop(pc)` требует выделенного FreeRTOS task с тиком 1 мс (stack 8192, priority 6, core 0) — без него ICE, DTLS и SCTP не обрабатываются. Состояние `PEER_CONNECTION_COMPLETED` (не CONNECTED) означает готовность к отправке видео и DataChannel-сообщений.
- **Процесс-lifecycle после XR-сессии**: Quest OS убивает Activity, запущенную в «тёплом» процессе (процесс уже имел XR-сессию) — сессия застревает на `XR_SESSION_STATE_SYNCHRONIZED`, никогда не достигает `FOCUSED`, и через ~300 мс приходит `Force finishing activity`. Свежий процесс (Zygote fork) всегда работает. Решение: `onDestroy()` вызывает `Process.killProcess(myPid())` — следующий запуск всегда получает чистый процесс. Дополнительно: если OS убивает процесс до `onDestroy()` (за ~29 мс), `onPause()` ставит `AlarmManager`-alarm на 2 с (живёт в system_server, выживает после смерти процесса); условие срабатывания — `isFinishing && xrThread != null && !nativeWasSessionFocused()`. Флаг `sessionEverFocused` в `XrApp` (C++) сбрасывается в `xrApp_destroy()` чтобы не наследовать значение `true` от предыдущей сессии при переиспользовании процесса.
