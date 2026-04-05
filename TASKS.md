# TASKS.md

Файл для трекинга задач разработки FPV WebRTC проекта.

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
