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
| `datachannel.js` | `FPVDataChannel.kt` | ✅ TASK-005 (bind + clock sync; E2E в status bar) |
| *(нет аналога)* | `EglVideoSink.kt` + `video_decoder.cpp` | ✅ TASK-004 |
| `webxr-renderer.js` | `xr_renderer.cpp` + `XrRenderThread.kt` | ✅ TASK-005 |
| `stats.js` | inline в `MainActivity.kt` | ✅ TASK-005 (basic E2E display) |

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
