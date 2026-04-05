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

**public/index.html** — Страница вьювера для Quest 2. Загружает четыре JS-модуля:

- **webrtc-client.js** — Управляет RTCPeerConnection. Регистрируется как "viewer", получает offer, создаёт answer, обрабатывает ICE. Переставляет SDP для приоритета H.264 (аппаратное ускорение на Quest 2).
- **webxr-renderer.js** — Стерео-рендеринг через WebGL2 + WebXR. Отрисовывает входящее видео на полноэкранный quad дважды (левый/правый глаз) со смещением viewport. IPD: 0.064 м, FOV: 90°, виртуальное расстояние: 1.5 м. Экспортирует кватернион головы для будущего управления сервой.
- **stats.js** — Опрашивает `RTCPeerConnection.getStats()` каждую секунду для отображения задержки, FPS, разрешения и кодека в UI.
- **main.js** — Оркестрирует все модули. Управляет переходами состояний UI (connecting → waiting → streaming). Передаёт входящий MediaStream как на плоское видео, так и в WebXR-рендерер.

## Статистика

Модуль `stats.js` опрашивает `RTCPeerConnection.getStats()` каждую секунду и отображает:

| Метрика | Источник |
|---------|----------|
| Сеть | RTT/2 из `candidate-pair.currentRoundTripTime` |
| Декод | `totalDecodeTime / framesDecoded` за интервал |
| Буфер | `jitterBufferDelay / jitterBufferEmittedCount` |
| Итого | сумма трёх выше (задержка на стороне получателя) |

Статистика выводится в трёх местах:
- **Полоска** под кнопками — всегда при активном потоке
- **Flat overlay** (`#flat-stats`) — поверх видео в режиме "Смотреть на экране"
- **WebGL HUD** — квад в world-space поверх видео в VR-режиме (рендерится через те же XR view/projection матрицы, обновляется внутри XR-фрейма через `_uploadStatsTexture`)

## Ключевые технические ограничения

- WebXR требует HTTPS (`TLS=1`), кроме случая `localhost`.
- TLS-сертификаты должны находиться по путям `server/certs/key.pem` и `server/certs/cert.pem` до запуска с `TLS=1`. При их отсутствии сервер выводит ошибку и завершается.
- Сервер выставляет заголовки безопасности `Cross-Origin-Embedder-Policy: require-corp` и `Cross-Origin-Opener-Policy: same-origin` (нужно для SharedArrayBuffer).
- STUN: `stun.l.google.com:19302`. TURN-сервер отсутствует — оба пира должны быть в одной LAN или иметь прямую связность.
- Смещение глаз в `webxr-renderer.js` захардкожено (0.03 м на глаз), не учитывает перспективу.
- DataChannel для передачи позы головы на сервоуправление — запланированная, но ещё не реализованная функция (TODO в `main.js`).
