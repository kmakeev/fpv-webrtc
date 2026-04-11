# FPV WebRTC Viewer для Quest 2

Проект для просмотра FPV-потока с ESP32-P4 (или ноутбука для тестирования)
в стерео-режиме через Oculus Quest 2.

---

## Структура проекта

```
fpv-webrtc/
├── server/
│   └── server.js          ← Node.js сервер (сигналинг + статика)
├── public/
│   ├── index.html         ← Quest 2 открывает эту страницу
│   ├── streamer.html      ← Ноутбук открывает эту страницу (тест)
│   ├── css/style.css
│   └── js/
│       ├── webrtc-client.js   ← WebRTC соединение
│       ├── webxr-renderer.js  ← WebGL + WebXR стерео-рендеринг
│       ├── stats.js           ← Статистика (задержка, FPS, кодек)
│       └── main.js            ← Основная логика
├── gen-certs.sh           ← Генерация TLS-сертификата (для WebXR)
└── package.json
```

---

## Шаг 1 — Установка Node.js и зависимостей (ноутбук)

### Windows
1. Скачать Node.js LTS: https://nodejs.org/
2. Установить, перезапустить терминал
3. В папке проекта выполнить:
   ```
   npm install
   ```

### Linux / macOS
```bash
# Через nvm (рекомендуется):
curl -o- https://raw.githubusercontent.com/nvm-sh/nvm/v0.39.7/install.sh | bash
source ~/.bashrc
nvm install --lts

# Затем:
cd fpv-webrtc
npm install
```

---

## Шаг 2 — Генерация TLS-сертификата (обязательно для WebXR)

WebXR в браузере Quest 2 работает **только через HTTPS** (или localhost).
Нужно создать самоподписанный сертификат:

### Linux / macOS
```bash
chmod +x gen-certs.sh
./gen-certs.sh
```

### Windows (PowerShell)
```powershell
# Если openssl установлен (обычно есть с Git Bash):
bash gen-certs.sh

# Или через PowerShell напрямую (замените IP на IP ноутбука):
$ip = "192.168.1.100"
openssl req -x509 -newkey rsa:2048 -nodes `
  -keyout server/certs/key.pem `
  -out    server/certs/cert.pem `
  -days 365 `
  -subj "/CN=FPV-Server" `
  -addext "subjectAltName=IP:${ip},IP:127.0.0.1,DNS:localhost"
```

**Как узнать IP ноутбука:**
- Windows: `ipconfig` → "IPv4-адрес" (например 192.168.1.105)
- Linux:   `ip a` → строка `inet` интерфейса wlan0 или eth0

---

## Шаг 3 — Запуск сервера (ноутбук)

```bash
# С TLS (нужно для Quest 2):
TLS=1 node server/server.js

# Windows:
set TLS=1 && node server/server.js

# Без TLS (только для теста в браузере ноутбука):
node server/server.js
```

Вывод будет примерно таким:
```
✓ FPV Signaling Server запущен
  Локально:  https://localhost:8080
  В сети:    https://192.168.1.105:8080

  Quest 2: откройте браузер → https://192.168.1.105:8080
```

---

## Шаг 4 — Подключение Quest 2

### 4.1 — Убедиться что Quest 2 в той же Wi-Fi сети
Настройки Quest 2 → Wi-Fi → подключиться к той же точке доступа что и ноутбук.

### 4.2 — Открыть браузер в Quest 2
В Quest 2 нажать кнопку Meta → Приложения → **Meta Browser** (или Oculus Browser).

### 4.3 — Принять сертификат
Ввести в адресную строку: `https://192.168.1.105:8080` (IP своего ноутбука)

Браузер покажет предупреждение о сертификате — это нормально для самоподписанного:
- Нажать **"Дополнительно"**
- Нажать **"Перейти на сайт"** (или аналогичное)

Страница должна открыться и показать статус "Ожидание стримера...".

---

## Шаг 5 — Запуск тестового стримера (ноутбук)

Откройте в браузере ноутбука:
```
https://localhost:8080/streamer.html
```

Разрешите доступ к вебкамере. Страница покажет превью с камеры.

---

## Шаг 6 — Тест

1. На ноутбуке открыта страница `streamer.html` с видео с вебкамеры
2. На Quest 2 открыта страница `index.html`
3. Автоматически появятся кнопки **"Войти в VR"** и **"Смотреть на экране"**
4. Нажать **"Войти в VR"** — Quest перейдёт в иммерсивный режим
5. В шлеме вы увидите видео с вебкамеры ноутбука в стерео

---

## Шаг 7 — Настройка Developer Mode на Quest 2 (для sideloading APK в будущем)

Пока не нужно для этого теста, но понадобится позже:

1. Установить **Meta Quest** приложение на смартфон
2. Войти в аккаунт, выбрать свой Quest 2
3. Меню → **Настройки устройства**
4. Найти раздел **"Режим разработчика"** → включить
5. На Quest 2: при подключении USB к ноутбуку → разрешить USB debugging

---

## Нативное приложение для Quest 2 (fpv-native-quest/)

Нативный APK использует OpenXR + MediaCodec вместо браузера. Преимущества:
- Стерео рендеринг через OpenXR (реальные IPD/FOV шлема, ATW runtime)
- Zero-copy H.264 декодинг: MediaCodec → SurfaceTexture → OES-текстура → OpenXR
- E2E-задержка ~15–35 мс vs ~65–100 мс в браузере

### Текущий статус

| Задача | Статус | Что реализовано |
|--------|--------|-----------------|
| TASK-002 | ✅ | Скелет Android-проекта, Gradle + NDK |
| TASK-003 | ✅ | WebRTC сигналинг, H.264 декодинг, плоский SurfaceViewRenderer |
| TASK-004 | ✅ | Zero-copy OES-текстура (EglVideoSink → video_decoder.cpp) |
| TASK-005 | ✅ | OpenXR стерео-рендеринг (xr_renderer.cpp + XrRenderThread.kt) |
| TASK-006 | ✅ | FPVDataChannel E2E-статистика в VR HUD (world-space quad над видео, Bitmap→GL_TEXTURE_2D, recentering) |
| TASK-009 | ✅ | Grab & scale видео-окна: правый grip перемещает, правый стик Y масштабирует, recentre сбрасывает |

### Сборка и запуск

**Требования:** JDK 17, Android SDK с NDK 27.x и CMake 3.22.1.  
Все остальные зависимости (WebRTC SDK, OpenXR loader) Gradle скачивает с Maven Central автоматически.

```bash
cd fpv-native-quest/

# Первый запуск: создать local.properties
cp local.properties.template local.properties
# Проверить что sdk.dir указывает правильно:
cat local.properties   # sdk.dir=/Users/<user>/Library/Android/sdk

# Установить JAVA_HOME если JDK 17 не в системном PATH
export JAVA_HOME=/opt/homebrew/opt/openjdk@17   # macOS Homebrew

# Сборка — Gradle автоматически скачивает OpenXR loader (org.khronos.openxr:openxr_loader_for_android)
./gradlew assembleDebug                          # APK → app/build/outputs/apk/debug/
./gradlew installDebug                           # сборка + установка на Quest 2

# Запустить и смотреть логи
adb shell am start -n com.fpv.quest/.MainActivity
adb logcat -s FPVQuest WebRTCEngine SignalingClient FPVDataChannel xr_renderer video_decoder
```

> Подробная инструкция по установке окружения: [`docs/native-android-setup.md`](docs/native-android-setup.md)

### Подключение

1. Запусти сервер **без TLS**: `npm start`
2. В приложении Quest введи: `ws://192.168.x.x:8080` и нажми **Connect**
3. Открой `http://192.168.x.x:8080/streamer.html` в браузере на ноутбуке
4. Надень Quest — видео появится в стерео (OpenXR VR-режим)

> Если сервер запущен с `TLS=1`, вводи `wss://` — приложение принимает самоподписанный сертификат автоматически.

### Управление видео-окном (TASK-009)

| Действие | Жест |
|----------|------|
| Переместить окно | Правый grip — потянуть |
| Увеличить / уменьшить | Правый grip удержать + правый стик Y вверх / вниз |
| Сбросить положение | Кнопка Oculus (recentre) |
| Открыть настройки IP | Кнопка Y (левый контроллер) |
| Подтвердить IP | Кнопка A (правый контроллер) |

Ширина окна: 0.5–4.0 м; соотношение сторон 16:9 сохраняется. Stats HUD следует за окном автоматически.

---

## Диагностика

### Статус "Подключение..." не меняется
- Сервер запущен? Проверить терминал ноутбука
- Quest 2 и ноутбук в одной Wi-Fi? Проверить настройки
- Правильный IP в адресной строке? `ipconfig` / `ip a`

### Предупреждение о сертификате не исчезает
- Сертификат создан для правильного IP? Запустить `gen-certs.sh` заново
- Убедиться что IP в адресной строке Quest совпадает с IP в сертификате

### "immersive-vr не поддерживается"
- Убедиться что открыт именно **Meta Browser** (не сторонний)
- Обновить прошивку Quest 2 до последней версии

### Видео идёт но не в стерео
- WebXR режим требует нажатия кнопки "Войти в VR" внутри браузера Quest
- На ноутбуке WebXR недоступен — только "Смотреть на экране"

---

## Замена ноутбука на ESP32-P4

Когда приедёт плата — меняется только стример.
Со стороны Quest 2 **ничего не меняется**:

| Сейчас (тест) | С ESP32-P4 |
|---|---|
| Ноутбук запускает Node.js сервер | ESP32-P4 запускает esp-webrtc-solution |
| streamer.html захватывает вебкамеру | ESP32-P4 захватывает MIPI CSI камеру |
| Quest 2 открывает https://IP_ноутбука:8080 | Quest 2 открывает http://192.168.4.1 (SoftAP) |
| H.264 через браузер | H.264 через HW энкодер (~15ms) |

---

## Статистика соединения

После установки потока отображаются метрики задержки в трёх местах:

- **Основной интерфейс** — полоска под кнопками (показывает E2E если доступна, иначе Сеть)
- **Режим "Смотреть на экране"** — плашка в верхней части видео
- **VR-режим (браузер)** — WebGL HUD поверх картинки в шлеме
- **VR-режим (нативное приложение)** — world-space quad (GL_TEXTURE_2D Bitmap 256×64 px) над видео-экраном на расстоянии 2 м; обновляется через `XrRenderThread.updateStats()` → `GLUtils.texImage2D()` → `nativeSetStatsTexture()`; скрывается при разрыве DataChannel

---

### Откуда берётся каждая метрика

```
Стример (ноутбук / ESP32-P4)            Вьювер (Quest 2)
────────────────────────────            ────────────────
[Захват кадра]  ← capture timestamp
      │ энк
[H.264 энкодинг] ─── RTP по UDP ──►  [Jitter-буфер]
                                             │ буфер
                                       [Декодирование]
                                             │ декод
                                       [Рендеринг кадра]

[DataChannel 'ts'] ── SCTP ─────────►  [onTimestamp] → E2E
```

| Метрика | Где считается | Что измеряет | Источник данных |
|---------|--------------|--------------|-----------------|
| **Сеть** | Вьювер | RTT÷2 — оценка односторонней сетевой задержки | `candidate-pair.currentRoundTripTime` из `getStats()` |
| **Декод** | Вьювер | `totalDecodeTime/framesDecoded` — на desktop ~1ms, на Android/Quest ~38ms (включает async MediaCodec pipeline) | `getStats()` |
| **Буфер** | Вьювер | Время ожидания кадра в jitter-буфере | `jitterBufferDelay / jitterBufferEmittedCount` из `getStats()` |
| **Итого** | Вьювер | Сеть + Декод + Буфер | Сумма трёх выше |
| **Энк** | Стример | Время H.264 кодирования кадра | `encode_ts − capture_ts` из DataChannel |
| **E2E** | Стример→Вьювер | Задержка DataChannel (SCTP) сообщения ≈ сетевая задержка | DataChannel `viewer_now − capture + clockOffset` |

---

### Важное: E2E и Итого — независимые метрики, они не складываются

**Итого** считается полностью из данных на стороне вьювера (`RTCPeerConnection.getStats()`).  
Сеть = RTT÷2 — это **приближение**: предполагается симметричный канал, реальная односторонняя задержка может отличаться.

**E2E** измеряет задержку **DataChannel (SCTP) сообщения** от стримера до вьювера.  
Контрольное сообщение `{type:'ts'}` идёт по отдельному SCTP-потоку и **не проходит через jitter-буфер и декодер** — только через сеть.

Поэтому **E2E почти всегда меньше Итого**:

```
E2E     ≈  сетевая задержка (SCTP)             ← ~20ms в примере
Итого   =  RTT/2 + jitter-буфер + декод        ← ~52ms в примере (может завышать)
```

**Реальная задержка видеокадра** (от захвата до отображения) приблизительно равна:

```
Video E2E ≈ E2E_dc + Буфер + Декод
           ≈     20 +     9 +    38  =  ~67ms
```

---

### Почему Декод на Quest ~38ms, а на PC ~1ms

`totalDecodeTime` на **desktop Chromium** = чистое время синхронного декодера (ffmpeg или NVDEC/DXVA). Реальное, ~1ms.

`totalDecodeTime` на **Android/Quest** измеряется через асинхронный MediaCodec API: от отправки кадра в очередь декодера до получения колбэка о готовности. Включает ожидание в очереди, YUV→RGB конвертацию, передачу буфера GPU. Реальное время H.264-декодирования ~1–3ms, но API возвращает ~38ms (pipeline latency).

`requestVideoFrameCallback.processingDuration` возвращает ту же самую величину на Android — оба API опираются на один источник. Попытка использовать rVFC для улучшения точности Декод на Quest была проверена и отклонена: значения одинаковые, а колбэк 30 раз/сек добавлял нагрузку, что увеличивало jitter-буфер.

**На ESP32-P4** `Энк` покажет реальное время H.264-кодирования (~10–15ms аппаратного энкодера).

### Почему E2E < Итого

`E2E` измеряет задержку DataChannel (SCTP) сообщения, которое идёт напрямую по сети, минуя jitter-буфер и декодер видео. Поэтому E2E ≈ чистая сетевая задержка.

Реальная задержка видеокадра: `Video E2E ≈ E2E + Буфер + Декод`.

---

### Точность измерений

| Метрика | Точность | Ограничения |
|---------|----------|-------------|
| Сеть | ±1–2ms | RTT/2 — приближение, предполагает симметричный канал |
| Декод | ±1ms (desktop) / ±? (Android) | На Android включает MediaCodec pipeline ~38ms вместо ~2ms реального декода |
| Буфер | ±1ms | Надёжная метрика |
| Итого | ±1ms (desktop) / завышен (Android) | Декод тянет Итого вверх на Quest |
| E2E | ±2–5ms | SCTP latency ≠ video frame latency; точность ограничена clock sync |
| Энк | 0ms для вебкамеры | Браузер не даёт доступ к времени энкода |

---

## Docker

### Требования

- [Docker Desktop](https://www.docker.com/products/docker-desktop/) (macOS / Windows) или Docker Engine + Docker Compose Plugin (Linux)

### HTTP-режим (локальная разработка)

```bash
docker compose --profile http up
```

Откройте `http://localhost:8080` (стример) и `http://localhost:8080/streamer.html` в браузере.

### HTTPS/TLS-режим (обязательно для WebXR на Quest 2)

Сертификаты нужно сгенерировать на хосте — они содержат IP хоста, который Docker не знает:

```bash
./gen-certs.sh
docker compose --profile tls up
```

На Quest 2 откройте `https://<IP_ноутбука>:8080`.

> **Принятие самоподписанного сертификата на Quest 2:**
> Meta Browser покажет предупреждение — нажмите «Дополнительно» → «Перейти на сайт».
> Это нужно сделать один раз; после этого WebXR работает штатно.

### Изменение порта

```bash
PORT=9090 docker compose --profile http up
```
