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

## Ключевые технические ограничения

- WebXR требует HTTPS (`TLS=1`), кроме случая `localhost`.
- TLS-сертификаты должны находиться по путям `server/certs/key.pem` и `server/certs/cert.pem` до запуска с `TLS=1`. При их отсутствии сервер выводит ошибку и завершается.
- Сервер выставляет заголовки безопасности `Cross-Origin-Embedder-Policy: require-corp` и `Cross-Origin-Opener-Policy: same-origin` (нужно для SharedArrayBuffer).
- STUN: `stun.l.google.com:19302`. TURN-сервер отсутствует — оба пира должны быть в одной LAN или иметь прямую связность.
- Смещение глаз в `webxr-renderer.js` захардкожено (0.03 м на глаз), не учитывает перспективу.
- DataChannel реализован (`datachannel.js`): синхронизация часов и E2E-задержка работают. Передача позы головы (`type:'head'`) отключена — управление камерой будет через контроллеры Quest.
- `syncClocks()` запускается через 3 с после открытия DataChannel — иначе 5 быстрых SCTP ping-pong раскачивают jitter-буфер с ~19ms до ~40ms.
- Стример (`streamer.html`) отправляет `type:'ts'` с `capture = encode = Date.now()` — энкод-задержка вебкамеры в браузере недоступна; для ESP32-P4 это поле будет содержать реальное время H.264-кодирования.
- Метка времени захвата (`capture`) — `Date.now()` в момент отправки `ts`-сообщения, не RTP-timestamp.
- `totalDecodeTime` на Android/Quest включает async MediaCodec pipeline latency (~38ms), а не только декодирование (~2ms). `requestVideoFrameCallback.processingDuration` возвращает ту же величину — rVFC отклонён (добавляет нагрузку без улучшения точности).
