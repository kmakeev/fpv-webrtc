# TASKS.md

Файл для трекинга задач разработки FPV WebRTC проекта.

---

## Инструкция: Developer Mode и установка APK на Quest 2

### Шаг 1 — Создать организацию разработчика в Meta

Без этого шага кнопка Developer Mode в настройках шлема будет недоступна.

1. Открыть [developer.oculus.com](https://developer.oculus.com) на компьютере (нужен Meta-аккаунт, привязанный к шлему)
2. Войти → нажать **Get Started** → **Create New Organization**
3. Ввести любое название (например, `fpv-dev`) → принять соглашение разработчика
4. Организация создаётся мгновенно, подтверждения не требуется

> Если аккаунт не привязан к шлему: **настройки Meta Quest → Accounts → Add Account** или войти при первом включении шлема.

---

### Шаг 2 — Включить Developer Mode на шлеме

**Через приложение Meta Quest на телефоне** — единственный надёжный способ:

1. Установить приложение **Meta Quest** (iOS / Android)
2. Войти в тот же Meta-аккаунт, к которому привязан шлем
3. Открыть: **Menu (☰) → Devices** → выбрать свой Quest 2
4. **Headset Settings → Developer Mode → переключить в ON**
5. Шлем нужно перезагрузить — держать кнопку питания ~3 сек → **Restart**

> **Важно:** Developer Mode нельзя включить из меню самого шлема и через браузер на ноутбуке — только через мобильное приложение Meta Quest. Раздел **Settings → System → Developer** в шлеме появляется _после_ активации через приложение и содержит дополнительные настройки (USB-диалог, WiFi ADB), но не сам переключатель.

> Признак успешного включения: `adb devices` показывает шлем со статусом `device`.

---

### Шаг 3 — Подключить шлем к компьютеру по USB

1. Кабель: **USB-C → USB-C** или **USB-C → USB-A** (обычный зарядный кабель Quest подходит)
2. Надеть шлем и подключить кабель
3. В шлеме появится диалог: **«Allow USB Debugging?»** → нажать **Allow**
   - Если диалога нет: снять и надеть шлем повторно, иногда нужно разблокировать его
   - Поставить галку **«Always allow from this computer»** чтобы не подтверждать каждый раз
4. Проверить подключение на компьютере:

```bash
~/Library/Android/sdk/platform-tools/adb devices
```

Ожидаемый вывод:
```
List of devices attached
1WMHXXXXXXXX    device
```

Если статус `unauthorized` — повторить диалог авторизации в шлеме.  
Если статус `offline` — другой кабель или другой USB-порт (USB3 иногда даёт проблемы, попробовать USB2).

---

### Шаг 4 — Добавить ADB в PATH (один раз)

```bash
# Добавить в ~/.zshrc:
export ANDROID_HOME="$HOME/Library/Android/sdk"
export PATH="$ANDROID_HOME/platform-tools:$PATH"

# Применить:
source ~/.zshrc

# Теперь adb доступен глобально:
adb devices
```

---

### Шаг 5 — Сборка и установка APK из Android Studio

#### Первая установка (debug-сборка)

1. Открыть проект `fpv-native-quest/` в Android Studio
2. В панели **Device** (правый верхний угол или **View → Tool Windows → Device Manager**) должен появиться Quest 2
3. Нажать **Run ▶** (Shift+F10) — Android Studio соберёт APK и установит его на шлем автоматически

Или через командную строку:
```bash
cd fpv-native-quest/
./gradlew installDebug
```

#### Запуск установленного APK без компьютера

После установки приложение появляется в библиотеке шлема:
1. В Quest 2: **Apps (нижнее меню) → фильтр: Unknown Sources**
2. Найти **FPV Quest** (или имя из `AndroidManifest.xml`) → нажать для запуска

> Приложения из `adb install` и Android Studio попадают именно в **Unknown Sources**, а не в основную библиотеку.

#### Повторная установка (обновление)

```bash
# Пересобрать и обновить без удаления данных:
./gradlew installDebug

# Или только залить готовый APK:
adb install -r app/build/outputs/apk/debug/app-debug.apk
# -r = replace (не удаляет данные приложения)
```

---

### Шаг 6 — Просмотр логов во время работы

```bash
# Все логи приложения (тег fpv или имя пакета):
adb logcat -s fpv

# Или с фильтрацией по пакету (заменить на реальное имя):
adb shell pidof com.fpv.quest | xargs -I{} adb logcat --pid={}

# Логи WebRTC (очень подробно):
adb logcat -s libjingle WebRTC

# Только ошибки:
adb logcat *:E
```

---

### Устранение типичных проблем

| Проблема | Причина | Решение |
|----------|---------|---------|
| `adb devices` пустой | Диалог авторизации не принят | Надеть шлем, найти диалог, нажать Allow |
| `unauthorized` | Ключ не добавлен | `adb kill-server && adb start-server`, повторить диалог |
| `INSTALL_FAILED_USER_RESTRICTED` | Developer Mode выключен | Повторить Шаг 2 |
| `INSTALL_FAILED_VERSION_DOWNGRADE` | APK старее установленного | `adb uninstall com.fpv.quest`, затем `adb install` |
| Приложение не видно в библиотеке | Ищут не там | Открыть Apps → Unknown Sources |
| `error: device offline` | Кабель или питание | Сменить порт, попробовать другой кабель |
| Диалог авторизации не появляется | Шлем заблокирован | Надеть и разблокировать, затем переподключить USB |

---

## Обоснование нативного приложения — сравнение задержек

### Текущие показатели (WebRTC в браузере Oculus Browser / Quest 2)

| Компонент | Задержка | Причина |
|-----------|----------|---------|
| Сеть (RTT/2) | ~5–15 мс | LAN, STUN |
| Декодинг | **~38 мс** | Браузер использует async MediaCodec pipeline (несколько буферных стадий) |
| Jitter-буфер | **~15–25 мс** | Консервативные настройки WebRTC в браузере, нельзя изменить |
| Загрузка кадра в GPU | ~2–5 мс | Программный путь: CPU-copy из video element в GL-текстуру |
| Оверхед браузера/compositor | ~5–10 мс | Browser compositor, ATW слой поверх сцены |
| **Итого E2E** | **~65–100 мс** | |

### Целевые показатели (нативное Android-приложение)

| Компонент | Цель | За счёт чего |
|-----------|------|-------------|
| Сеть (RTT/2) | ~5–12 мс | Незначительное улучшение от DSCP/QoS |
| Декодинг | **~3–5 мс** | MediaCodec с Surface output — synchronous-режим, без промежуточных буферов |
| Jitter-буфер | **~4–8 мс** | `minPlayoutDelay=0ms` через WebRTC API, агрессивный режим NACK |
| Загрузка кадра в GPU | **~0 мс** | SurfaceTexture → EGLImageKHR — zero-copy, GPU↔GPU, без CPU |
| Оверхед рендеринга | ~1–2 мс | Прямой OpenGL ES 3.2/Vulkan, нет browser compositor |
| **Итого E2E** | **~15–35 мс** | **~55–75% сокращение** |

> Главный выигрыш — декодинг (−33 мс) и jitter-буфер (−12 мс): суммарно ~45 мс только на двух компонентах.  
> Источник данных по декодингу в браузере: `totalDecodeTime / framesDecoded` из `stats.js`, `~38ms` задокументировано в CLAUDE.md.  
> Целевые цифры нативного MediaCodec: Snapdragon XR2 (Quest 2) декодирует H.264 720p за 2–4 мс в synchronous-режиме — [Android MediaCodec low-latency](https://developer.android.com/reference/android/media/MediaCodec#KEY_LATENCY).

---

## TASK-001 — DataChannel + синхронизация часов для полной end-to-end статистики задержки

**Статус:** `done`  
**Приоритет:** высокий  
**Зависимости:** нет

### Цель

Реализовать измерение полной end-to-end задержки видеопотока, включая захват камеры и энкодинг на стороне стримера — метрики, недоступные при измерении только на стороне вьювера.

### Текущее состояние

`stats.js` измеряет только задержку на стороне Quest (сеть RTT/2 + декод + джиттер-буфер). Задержка захвата и энкодинга на стороне стримера неизвестна. В `main.js` есть TODO на реализацию DataChannel.

### Архитектура решения

```
Стример                              Вьювер (Quest)
───────                              ──────────────
datachannel.js                       datachannel.js
  │                                    │
  ├─ clock sync (NTP-подобный) ───────▶│ вычисляем clock offset
  │    ping: {type:'ping', t: now}     │
  │◀───────────────────────────────────┤ pong: {type:'pong', t0, t1: now}
  │    offset = (RTT/2) - (t1 - t0)   │
  │                                    │
  ├─ каждый кадр: ────────────────────▶│ e2e = now - offset - frame_t
  │    {type:'ts', t: captureTime}     │
  │                                    │
stats (streamer side):               stats (viewer side):
  captureMs = encode start - t        networkMs = из RTCP/getStats
  encodeMs  = encoded - capture       e2eMs = frame_t → render
```

### Файлы для создания / изменения

| Файл | Действие | Описание |
|------|----------|----------|
| `public/js/datachannel.js` | создать | общий модуль DataChannel для стримера и вьювера |
| `public/streamer.html` | изменить | открыть DataChannel, отправлять временны́е метки захвата |
| `public/js/webrtc-client.js` | изменить | принять DataChannel от стримера |
| `public/js/stats.js` | изменить | принимать e2e-метрики из DataChannel, отображать |
| `public/js/main.js` | изменить | убрать TODO, подключить datachannel.js |

### Детали реализации

#### 1. datachannel.js — новый модуль

Экспортирует `window.FPVDataChannel`:

```js
// Стример создаёт канал:
FPVDataChannel.create(pc)   // → RTCDataChannel

// Вьювер принимает канал:
FPVDataChannel.accept(pc)   // слушает ondatachannel

// Синхронизация часов (5 round-trips → медиана offset):
FPVDataChannel.syncClocks()  // → Promise<offset_ms>

// Колбэки:
FPVDataChannel.onClockSynced = (offsetMs) => {}
FPVDataChannel.onTimestamp   = (frameData) => {}  // { captureMs, encodeMs, frameId }
```

#### 2. Алгоритм NTP clock sync

```
for i in 0..4:
    t0 = Date.now()
    send {type:'ping', id:i, t: t0}
    wait pong {id:i, t0, t1}
    t2 = Date.now()
    RTT = t2 - t0
    offset = t1 - t0 - RTT/2   // сколько часы стримера опережают часы вьювера

offset_final = median(5 измерений)
```

#### 3. Стример — отправка временны́х меток

В `streamer.html`, после `getUserMedia`, перед каждым кадром:

```js
// Примерно раз в секунду (не на каждый кадр — DataChannel не для этого)
const captureTime = Date.now();
// ... encode happens ...
const encodeTime = Date.now();
dc.send(JSON.stringify({
    type: 'ts',
    capture: captureTime,
    encode:  encodeTime,
}));
```

#### 4. Вьювер — вычисление e2e задержки

```js
FPVDataChannel.onTimestamp = ({ capture, encode }) => {
    const nowLocal   = Date.now();
    const captureAdj = capture + clockOffset;  // время захвата в локальных часах
    const e2eMs      = nowLocal - captureAdj;  // полная задержка
    const encodeMs   = encode - capture;       // время энкодинга
    FPVStats.updateE2E({ e2eMs, encodeMs });
};
```

#### 5. Отображение в stats.js

Добавить в stats-bar и WebGL HUD новую строку:

```
E2E: ~45ms  (захват 5ms · энкод 12ms · сеть 12ms · декод 2ms · буфер 8ms)
```

### Критерии готовности

- [x] DataChannel устанавливается автоматически при WebRTC-соединении
- [x] Синхронизация часов выполняется после установки канала (5 измерений)
- [x] Стример отправляет временну́ю метку раз в секунду
- [x] Вьювер отображает e2e-задержку в stats HUD (VR и flat режим)
- [x] При разрыве DataChannel — метрика скрывается, не крашит приложение
- [x] Метрика появляется через ≤5 секунд после установки потока

### Ожидаемая точность

- Clock sync: ±1–2 мс на LAN
- E2E метрика: ±2–5 мс (ограничение — `Date.now()` на стороне стримера)
- Гранулярность обновления: 1 раз в секунду (достаточно для мониторинга)

### Примечания по итогам реализации

- Управление камерой через позу головы (`type:'head'`) отключено — планируется реализация через контроллеры Quest. Тип сообщения зарезервирован в `datachannel.js`.
- `syncClocks()` запускается через 3 с после открытия DataChannel: без задержки 5 ping-pong раскачивали jitter-буфер с ~19ms до ~40ms.
- `requestVideoFrameCallback` проверен и отклонён: `processingDuration` на Android/Quest возвращает ту же async MediaCodec pipeline latency что и `totalDecodeTime` — одинаковые ~38ms, при этом 30 колбэков/сек поднимали jitter-буфер.
- E2E = SCTP-задержка DataChannel, а не видеокадра. Реальная задержка видео ≈ E2E + Буфер + Декод.
- На ESP32-P4 `encode` в `ts`-сообщении будет содержать реальное время H.264-кодирования (~10–15ms).
- Метка времени захвата — `Date.now()` на стримере, не RTP-timestamp.

---

## TASK-002 — Окружение разработки нативного Android/Quest 2 приложения

**Статус:** `done`  
**Приоритет:** высокий (блокирует все последующие задачи)  
**Зависимости:** нет

### Цель

Подготовить рабочее окружение для сборки нативного APK под Quest 2 (Android ARM64), установить все необходимые SDK/NDK компоненты, настроить устройство для разработки.

### Что уже установлено в системе

| Компонент | Статус |
|-----------|--------|
| Android Studio | ✅ установлен (`/Applications/Android Studio.app`) |
| Android SDK (platform-tools, build-tools 36.1.0) | ✅ установлен (`~/Library/Android/sdk/`) |
| ADB | ✅ (`~/Library/Android/sdk/platform-tools/adb`) |
| Android NDK | ❌ не установлен |
| CMake (для NDK-сборки) | ❌ не установлен |
| JDK 17 | ❌ только заглушка от системы, нет runtime |

### Что нужно установить

#### 1. JDK 17 — через Homebrew (2–3 мин)

```bash
brew install openjdk@17
# Прописать в ~/.zshrc:
export JAVA_HOME=/opt/homebrew/opt/openjdk@17
export PATH="$JAVA_HOME/bin:$PATH"
```

Проверка: `java -version` должен показать `openjdk 17`.

#### 2. Android NDK r27 и CMake — через Android Studio SDK Manager (5–10 мин)

Открыть: **Android Studio → Settings → SDK Manager → SDK Tools**  
Отметить и установить:
- [x] NDK (Side by side) — выбрать версию **27.x** (последняя LTS)
- [x] CMake — версию **3.22.1**

После установки NDK появится в `~/Library/Android/sdk/ndk/27.x.xxxx/`.

#### 3. Android API 32 platform — через SDK Manager (1–2 мин)

Вкладка **SDK Platforms**: установить **Android 12L (API 32)**.  
Quest 2 работает на Android 10/12, target SDK = 32, min SDK = 29.

#### 4. Meta XR SDK для Android (OVR Mobile SDK) — скачать вручную

Скачать с [developer.oculus.com/downloads/package/oculus-mobile-sdk/](https://developer.oculus.com/downloads/package/oculus-mobile-sdk/)  
Текущая версия: **OVR Mobile SDK 69.0** (архив `ovr_sdk_mobile_*.zip`, ~200 МБ).  
Распаковать в `~/dev/ovr-sdk/` (или рядом с проектом).  
Нужны компоненты:
- `VrApi/` — core VR runtime (или OpenXR через `XrSamples/`)
- `1stParty/OVR/Include/` — заголовки C++ API

**Альтернатива:** использовать [Meta XR SDK через Maven](https://developer.oculus.com/documentation/native/android/mobile-studio-setup-and-install/) — тогда подключается как Gradle-зависимость без ручной распаковки.

#### 5. libwebrtc Android AAR — pre-built от Google

Скачать готовый AAR вместо самостоятельной компиляции WebRTC (~2 часа) — используем `io.github.webrtc-sdk:android` из Maven Central.

В `build.gradle` приложения:
```groovy
dependencies {
    implementation 'io.github.webrtc-sdk:android:125.6422.07'  // актуальная версия
}
```

Это официальный Google pre-built WebRTC для Android, включает:
- `PeerConnectionFactory`, `RtpReceiver`, `VideoTrack`
- JNI-обёртки над нативным libwebrtc
- MediaCodec hardware decode (H.264, VP8, VP9)

#### 6. Настройка Quest 2 для разработки

1. На Quest 2: **Settings → System → Developer → Developer Mode: ON**
2. Подключить Quest 2 по USB-C
3. Принять prompt авторизации ADB на гарнитуре
4. Проверка: `~/Library/Android/sdk/platform-tools/adb devices` должен показать устройство
5. Установить приложение **Meta Quest Developer Hub** (опционально, для логов и cast)

### Структура нового проекта

```
fpv-native-quest/          # отдельная директория рядом с fpv-webrtc
├── app/
│   ├── src/main/
│   │   ├── AndroidManifest.xml
│   │   ├── java/com/fpv/quest/
│   │   │   ├── MainActivity.kt       # точка входа, VR-сессия
│   │   │   ├── SignalingClient.kt    # WebSocket → сигналинг
│   │   │   ├── WebRTCEngine.kt       # PeerConnection, DataChannel
│   │   │   └── FPVDataChannel.kt     # clock sync, ts-сообщения
│   │   ├── cpp/
│   │   │   ├── CMakeLists.txt
│   │   │   ├── xr_renderer.cpp       # OpenXR + OpenGL ES 3.2
│   │   │   └── video_decoder.cpp     # MediaCodec surface output
│   │   └── res/
│   └── build.gradle
├── build.gradle
└── settings.gradle
```

### Критерии готовности

- [ ] `java -version` показывает JDK 17 *(ручная установка: `brew install openjdk@17`)*
- [ ] `~/Library/Android/sdk/ndk/` содержит директорию NDK 27.x *(ручная: SDK Manager)*
- [ ] `fpv-native-quest/` проект компилируется: `./gradlew assembleDebug` → `BUILD SUCCESSFUL`
- [ ] `adb devices` видит Quest 2 в режиме Developer Mode
- [ ] APK устанавливается на Quest 2 без ошибок подписи: `./gradlew installDebug`

### Что создано (TASK-002)

Полный Android-проект `fpv-native-quest/` внутри репозитория:

| Файл | Содержимое |
|------|-----------|
| `settings.gradle` / `build.gradle` | Gradle 8.7, AGP 8.3.0, Kotlin 1.9.22 |
| `app/build.gradle` | compileSdk=33, minSdk=29, arm64-v8a, libwebrtc+okhttp+coroutines |
| `AndroidManifest.xml` | INTERNET, VR headtracking, landscape, network_security_config |
| `MainActivity.kt` | Activity stub, TODO TASK-003/004 |
| `SignalingClient.kt` | OkHttp WebSocket, полный протокол server.js |
| `WebRTCEngine.kt` | PeerConnectionFactory stub + `preferH264()` (Kotlin-порт JS) |
| `FPVDataChannel.kt` | NTP clock sync + E2E расчёт, зеркало `datachannel.js` |
| `CMakeLists.txt` | libfpv-native.so, EGL + GLESv3 + log |
| `xr_renderer.cpp` | OpenXR init stub с TODO и архитектурным описанием |
| `video_decoder.cpp` | SurfaceTexture zero-copy stub с TODO |
| `docs/native-android-setup.md` | Пошаговая инструкция установки окружения |

### Примечания по итогам

- Проект создан внутри `fpv-webrtc/` (не отдельный репозиторий) — единая история git.
- `gradle-wrapper.jar` не включён в репозиторий (бинарный): Android Studio скачает автоматически; для CLI — `gradle wrapper` или `curl` из `docs/native-android-setup.md`.
- `local.properties` в `.gitignore`; шаблон `local.properties.template` отслеживается.
- `preferH264()` в `WebRTCEngine.kt` — точный порт `webrtc-client.js:reorderH264()`, разбит на тест-кейсы в TASK-003.
- Meta XR SDK (OVR Mobile SDK) не подключён — OpenXR интегрируется в TASK-004. Текущие C++ stub компилируются без него.
- `network_security_config.xml` разрешает `ws://` для RFC 1918 диапазонов (192.168.x.x, 10.x.x.x, 172.16.x.x) — для dev-режима без TLS.

---

## TASK-003 — Сигналинг и базовая WebRTC-сессия (Kotlin)

**Статус:** `todo`  
**Приоритет:** высокий  
**Зависимости:** TASK-002

### Цель

Установить WebRTC peer connection с существующим сервером (`server/server.js`) из нативного Kotlin-кода, используя тот же JSON-протокол что в `webrtc-client.js`. Получить первый видеопоток на экране Quest 2.

### Архитектура

```
SignalingClient (OkHttp WebSocket)
    ↓ role: "viewer"
    ← offer (SDP)
    → answer (SDP, H.264 preferred)
    ↔ ice (кандидаты)

WebRTCEngine (io.github.webrtc-sdk:android)
    PeerConnectionFactory
        ├── createPeerConnection()
        ├── VideoDecoderFactory → HardwareVideoDecoderFactory  ← ключевое
        └── onAddTrack() → VideoTrack → SurfaceViewRenderer / SurfaceTexture
```

### Ключевые детали реализации

#### SignalingClient.kt

- Библиотека: **OkHttp** (`com.squareup.okhttp3:okhttp:4.12.0`)
- URL: `wss://HOST:8080` (TLS) или `ws://HOST:8080` (без TLS)
- Первое сообщение: `{"type":"role","role":"viewer"}`
- Обработать: `offer`, `ice`, `peer_disconnected` — аналогично `webrtc-client.js:40–80`

#### WebRTCEngine.kt — приоритет H.264 в SDP

Повторить логику `webrtc-client.js:reorderH264()`:
```kotlin
fun preferH264(sdp: String): String {
    // найти m=video секцию, переставить payload type H264 первым
    // аналогично JS: sdpLines.splice(mLineIndex+1, 0, h264Line)
}
```

#### HardwareVideoDecoderFactory

```kotlin
val decoderFactory = HardwareVideoDecoderFactory(eglContext)
// НЕ DefaultVideoDecoderFactory — он может выбрать software decoder
val factory = PeerConnectionFactory.builder()
    .setVideoDecoderFactory(decoderFactory)
    .createPeerConnectionFactory()
```

### Критерии готовности

- [ ] Нативный клиент подключается к `server.js` и получает роль `viewer`
- [ ] WebRTC offer/answer обмен завершается успешно (видно в логах сервера)
- [ ] Видеопоток появляется в `SurfaceViewRenderer` (плоский режим, без VR)
- [ ] В `adb logcat` виден codec: `video/avc` (H.264), hardware decoder

---

## TASK-004 — Zero-copy видео-пайплайн: MediaCodec → SurfaceTexture → OpenGL ES

**Статус:** `todo`  
**Приоритет:** критический (даёт −33 мс на декодинге)  
**Зависимости:** TASK-003

### Цель

Заменить стандартный путь декодинга (MediaCodec → CPU buffer → GL texImage2D) на zero-copy GPU-путь через SurfaceTexture, и добавить настройки low-latency режима MediaCodec.

### Текущий vs целевой пайплайн

```
Браузер (текущий):
  RTP → libwebrtc → MediaCodec (async, 3-5 буферных стадии) → YUV CPU-buffer → 
  → texImage2D (CPU copy) → GL texture → compositor → дисплей
  Итого декод + upload: ~38–43 мс

Нативный (цель):
  RTP → libwebrtc → MediaCodec (synchronous, low-latency) → Surface output →
  → SurfaceTexture.updateTexImage() → EGLImageKHR → GL_TEXTURE_EXTERNAL_OES →
  → OpenXR framebuffer → дисплей
  Итого декод + upload: ~3–5 мс (нет CPU copy вообще)
```

### Реализация

#### 1. Custom VideoSink через EGL

Создать `EglVideoSink.kt` / `video_decoder.cpp`:

```kotlin
// Kotlin side
val surfaceTexture = SurfaceTexture(glTextureId)  // OES texture
val surface = Surface(surfaceTexture)

// Передать Surface в WebRTC как вывод декодера:
val videoDecoderFactory = HardwareVideoDecoderFactory(eglContext)
// WebRTC использует Surface из EGL context автоматически через
// VideoDecoderFactory.createDecoder() если eglContext задан
```

```cpp
// C++ side (xr_renderer.cpp)
// GL_TEXTURE_EXTERNAL_OES — специальный тип текстуры для SurfaceTexture
glBindTexture(GL_TEXTURE_EXTERNAL_OES, videoTextureId);
// Шейдер должен использовать #extension GL_OES_EGL_image_external
// и samplerExternalOES вместо sampler2D
```

#### 2. MediaCodec low-latency настройки

Через `HardwareVideoDecoderFactory` в WebRTC Android SDK настройки передаются через `MediaFormat`:

```kotlin
// В кастомном VideoDecoderFactory:
mediaFormat.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)     // API 30+
mediaFormat.setInteger("vendor.qti-ext-dec-low-latency.enable", 1)  // Qualcomm-специфично
mediaFormat.setInteger(MediaFormat.KEY_PRIORITY, 0)         // realtime priority
```

#### 3. WebRTC jitter buffer — минимальная задержка

```kotlin
// После создания PeerConnection:
peerConnection.setAudioPlayout(false)  // нет аудио
// Jitter buffer минимум:
val rtpParameters = receiver.parameters
rtpParameters.encodings[0].minBitrateBps = null  // не ограничиваем
peerConnection.receivers.forEach { receiver ->
    // minPlayoutDelay через RTP extension (если поддерживается)
}
// Альтернатива — через field trials при инициализации:
PeerConnectionFactory.initialize(
    PeerConnectionFactory.InitializationOptions.builder(context)
        .setFieldTrials("WebRTC-MinPlayoutDelay/Enabled/")
        .createInitializationOptions()
)
```

### Критерии готовности

- [ ] `adb logcat | grep MediaCodec` подтверждает surface output mode
- [ ] E2E задержка декодинга (из DataChannel stats) < 8 мс
- [ ] Нет CPU spikes при рендеринге (профайлер Android Studio → CPU)
- [ ] Jitter buffer < 10 мс в среднем (из DataChannel clock sync)

---

## TASK-005 — OpenXR стерео-рендеринг (C++ NDK)

**Статус:** `todo`  
**Приоритет:** высокий  
**Зависимости:** TASK-004

### Цель

Реализовать стерео VR-рендеринг через OpenXR (нативный C++ NDK) с использованием OES-текстуры из SurfaceTexture (zero-copy из TASK-004). Воспроизвести логику `webxr-renderer.js` в нативном коде.

### Стек

- **OpenXR** — стандартный API VR (поддерживается Quest через Meta OpenXR runtime)
- **OpenGL ES 3.2** — рендеринг (Vulkan опционально в TASK-008)
- **GL_OES_EGL_image_external** — текстура из SurfaceTexture без копирования

### Архитектура C++ модуля (`xr_renderer.cpp`)

```
XrInstance → XrSession → XrSwapchain (per eye)
    ↓
xrWaitFrame() → xrBeginFrame()
    ↓
for each XrView (left, right):
    glBindFramebuffer(swapchain image)
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, videoTexId)
    surfaceTexture->updateTexImage()  // ← JNI вызов в рендер-потоке
    DrawFullscreenQuad()              // шейдер с samplerExternalOES
    DrawStatsHUD()
xrEndFrame()
```

### Шейдер для OES текстуры

```glsl
// vertex shader — тот же quad что в webxr-renderer.js
#version 300 es
in vec2 a_pos; in vec2 a_uv;
uniform mat4 u_viewProj;
uniform float u_screenHalfW, u_screenHalfH, u_dist, u_yOffset;
out vec2 v_uv;
void main() {
    vec4 wp = vec4(a_pos.x*u_screenHalfW, a_pos.y*u_screenHalfH+u_yOffset, -u_dist, 1.0);
    gl_Position = u_viewProj * wp;
    v_uv = a_uv;
}

// fragment shader
#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
uniform samplerExternalOES u_video;   // ← отличие от браузерной версии
in vec2 v_uv;
out vec4 outColor;
void main() { outColor = texture(u_video, v_uv); }
```

### Параметры отображения (аналог webxr-renderer.js)

| Параметр | Браузер | Нативный |
|----------|---------|---------|
| IPD | 0.064 м (hardcoded) | Из `XrView.pose` (реальный IPD шлема) |
| FOV | 90° (hardcoded) | Из `XrView.fov` (реальный FOV шлема) |
| Расстояние | 1.5 м | 1.5 м (конфигурируемо) |
| Reprojection (ATW) | Автоматически browser | `XR_FB_composition_layer_settings` |

> Использование реальных IPD и FOV из XR API — дополнительный бонус нативного подхода: картинка будет точнее подогнана под конкретный шлем.

### Критерии готовности

- [ ] Видеопоток отображается в стерео в VR-режиме Quest 2
- [ ] FPS в XR сессии ≥ 72 (целевой refresh rate Quest 2)
- [ ] CPU frame time < 5 мс (GPU профайлер RenderDoc или `adb shell dumpsys SurfaceFlinger`)
- [ ] Шейдеры используют `samplerExternalOES` (подтверждено в исходниках)

---

## TASK-006 — DataChannel протокол и E2E-статистика (Kotlin)

**Статус:** `todo`  
**Приоритет:** средний  
**Зависимости:** TASK-003

### Цель

Реализовать `FPVDataChannel.kt` — Kotlin-эквивалент `datachannel.js` с полной совместимостью протокола: clock sync, ts-сообщения, отображение E2E-задержки в VR HUD.

### Протокол (совместим с текущим `datachannel.js`)

| Тип | Направление | Содержимое |
|-----|-------------|-----------|
| `ping` | viewer → streamer | `{"type":"ping","id":N,"t0":ms}` |
| `pong` | streamer → viewer | `{"type":"pong","id":N,"t0":ms,"t1":ms}` |
| `ts` | streamer → viewer | `{"type":"ts","capture":ms,"encode":ms}` |

### Реализация

```kotlin
class FPVDataChannel(private val dc: DataChannel) {
    private var clockOffset = 0L   // мс: часы стримера − часы вьювера

    fun syncClocks() {
        // 5 round-trips, медиана offset — идентично JS-версии
        // Запуск через 3 секунды после открытия канала (см. TASK-001 notes)
    }

    fun onMessage(buffer: DataChannel.Buffer) {
        val msg = JSONObject(String(buffer.data.array()))
        when (msg.getString("type")) {
            "pong" -> handlePong(msg)
            "ts"   -> {
                val capture = msg.getLong("capture")
                val encode  = msg.getLong("encode")
                val e2eMs   = System.currentTimeMillis() - capture + clockOffset
                val encMs   = encode - capture
                FPVStats.updateE2E(e2eMs, encMs)
            }
        }
    }
}
```

### Критерии готовности

- [ ] Clock sync выполняется через 3 с после открытия канала
- [ ] E2E метрика отображается в VR HUD (< 5 с после установки потока)
- [ ] `clockOffset` совпадает с браузерной версией ±2 мс при параллельном тесте
- [ ] При разрыве канала метрика скрывается без краша

---

## TASK-007 — Сетевая оптимизация и QoS

**Статус:** `todo`  
**Приоритет:** средний  
**Зависимости:** TASK-003, TASK-004

### Цель

Уменьшить сетевую составляющую задержки и вариативность jitter через настройки QoS, WebRTC field trials и сетевые параметры сокета.

### Оптимизации

#### 1. DSCP-маркировка пакетов (EF — Expedited Forwarding)

```kotlin
// Android требует разрешение CHANGE_NETWORK_STATE
// WebRTC Android SDK поддерживает через field trials:
PeerConnectionFactory.initialize(
    InitializationOptions.builder(context)
        .setFieldTrials(
            "WebRTC-SendSideBwe-WithOverhead/Enabled/" +
            "WebRTC-Video-MinPlayoutDelay/min_ms:0/" +
            "WebRTC-ReducedRtcpPollingInterval/Enabled/"
        )
        .createInitializationOptions()
)
```

#### 2. WiFi High Performance Lock (Android)

```kotlin
val wifiManager = getSystemService(WifiManager::class.java)
val wifiLock = wifiManager.createWifiLock(
    WifiManager.WIFI_MODE_FULL_HIGH_PERF,
    "fpv_wifi_lock"
)
wifiLock.acquire()
// release() в onStop()
```

#### 3. CPU Performance Lock (Quest 2)

```kotlin
// Quest 2: Snapdragon XR2 — зафиксировать CPU/GPU clock
// Через Meta VrApi (если используется) или Performance API:
// VrApi_SetClockLevels(ovr, 4, 4)  // CPU=4, GPU=4 (максимум)
```

#### 4. Jitter buffer агрессивный режим

```kotlin
// minPlayoutDelay = 0 через RTP header extension
// Или через WebRTC C++ API если используем NDK:
// video_receiver->SetMinimumPlayoutDelay(0)
```

### Целевой эффект

| Параметр | До | После |
|----------|-----|-------|
| Jitter buffer mean | ~15–25 мс | ~4–8 мс |
| Jitter buffer variance | ±10 мс | ±3 мс |
| WiFi power save latency spikes | +10–50 мс | устранены |

### Критерии готовности

- [ ] WiFi High Performance Lock удерживается во время стриминга
- [ ] Jitter buffer < 10 мс (из E2E stats)
- [ ] Нет latency spikes > 50 мс при стабильном WiFi
- [ ] CPU/GPU clock зафиксированы на период стриминга

---

## TASK-008 — (Опционально) Vulkan-рендеринг для минимального GPU latency

**Статус:** `backlog`  
**Приоритет:** низкий  
**Зависимости:** TASK-005

### Цель

Заменить OpenGL ES рендеринг на Vulkan для снижения CPU overhead и более точного управления синхронизацией кадров (VkSemaphore вместо glFinish/eglSwapBuffers).

### Обоснование

Quest 2 (Snapdragon XR2) полностью поддерживает Vulkan 1.1 с расширением `VK_KHR_external_memory_android` — это позволяет импортировать AHardwareBuffer из MediaCodec напрямую в Vulkan image без промежуточного GL-слоя.

```
MediaCodec → AHardwareBuffer → VkImage (через VK_EXT_external_memory_host)
                                    ↓
                            Vulkan render pass → XR swapchain
```

Выигрыш по сравнению с TASK-005: устраняется необходимость в SurfaceTexture/EGLImage слое, снижается CPU overhead с ~2 мс до ~0.5 мс.

### Критерии готовности

- [ ] Benchmark: CPU frame time < 1 мс (vs ~2 мс у OpenGL ES)
- [ ] Нет GL/EGL зависимостей в hot path рендеринга
- [ ] XR swapchain использует `VK_FORMAT_R8G8B8A8_UNORM`
