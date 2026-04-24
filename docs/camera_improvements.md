# Улучшения camera.c — ESP32-P4 FPV стример

> **Контекст:** файл `camera.c` реализует пайплайн OV5647 → MIPI CSI → ISP → HW H.264 → WebRTC.

## Статус: ✅ Все задачи реализованы

---

## Задача 1 — Добавить логирование реального FPS в camera_task

**Приоритет:** высокий | **Риск:** нулевой (только добавление логов)

**Проблема:** сейчас в логе видно только номер кадра каждые 30 кадров, без временно́й привязки.
Из лога следует что реальный fps ≈ 12, а не 50 заявленных сенсором — но это невозможно
увидеть без вычислений вручную.

**Что изменить** в функции `camera_task()`, в блоке heartbeat-логирования
(примерно строка 161, условие `task_frame_count % 30 == 0`):

```c
// Добавить статические переменные перед циклом while(1):
static int64_t s_fps_ts_us = 0;
static uint32_t s_fps_frame_ref = 0;

// Заменить строку ESP_LOGI с heartbeat на:
if (task_frame_count == 1 || task_frame_count % 30 == 0) {
    int64_t now_us = esp_timer_get_time();
    if (s_fps_ts_us > 0 && task_frame_count > s_fps_frame_ref) {
        float elapsed_s = (float)(now_us - s_fps_ts_us) / 1e6f;
        float fps = (float)(task_frame_count - s_fps_frame_ref) / elapsed_s;
        ESP_LOGI(TAG, "camera_task: frame #%"PRIu32" — %.1f fps (task-side)",
                 task_frame_count, fps);
    } else {
        ESP_LOGI(TAG, "camera_task: frame #%"PRIu32, task_frame_count);
    }
    s_fps_ts_us    = now_us;
    s_fps_frame_ref = task_frame_count;
}
```

**Ожидаемый результат:** в логе появятся строки вида
`camera_task: frame #90 — 12.3 fps (task-side)`, подтверждающие реальный fps
и позволяющие отслеживать эффект последующих оптимизаций.

---

## Задача 2 — Добавить rate-limit на обработку PLI/FIR запросов

**Приоритет:** высокий | **Риск:** минимальный

**Проблема:** из лога виден PLI-шторм сразу после WebRTC-подключения:

```
I (57205) camera: IDR force at frame #639
I (57209) webrtc: PLI/FIR received — requesting IDR
I (57262) camera: IDR force at frame #640   ← повторный IDR через 2ms
I (59221) camera: IDR force at frame #664   ← и ещё через 1.9 сек
```

Quest 2 запрашивает IDR быстрее, чем P4 успевает его отправить и доставить.
Каждый лишний IDR — тяжёлый кадр (~191 КБ из лога), перегружающий Wi-Fi и энкодер.

**Что изменить:** найти место в коде где вызывается `camera_request_idr()`
в ответ на PLI/FIR (в файле `webrtc.c` или аналогичном), обернуть вызов rate-limit'ом:

```c
// Добавить статическую переменную:
static int64_t s_last_idr_req_us = 0;

// Обернуть вызов camera_request_idr():
void on_pli_or_fir_received(void) {
    int64_t now_us = esp_timer_get_time();
    if ((now_us - s_last_idr_req_us) < 1500000LL) {   // не чаще раза в 1.5 секунды
        ESP_LOGD(TAG, "PLI/FIR: rate-limited, skip IDR request");
        return;
    }
    s_last_idr_req_us = now_us;
    ESP_LOGI(TAG, "PLI/FIR received — requesting IDR");
    camera_request_idr();
}
```

**Ожидаемый результат:** первые секунды после подключения стабилизируются,
исчезнут повторные IDR-запросы с интервалом < 1.5 сек.

---

## Задача 3 — Включить ISP AWB и AE контроллеры

**Приоритет:** высокий | **Риск:** низкий (добавление кода, не изменение существующего)

**Проблема:** в `camera_init()` ISP инициализируется и включается (`esp_isp_enable`),
но после этого не запускается ни один алгоритм автокоррекции. OV5647 — RAW-сенсор
без встроенного ISP, его выход без AWB/AE даёт зеленоватую картинку с неправильной
яркостью и балансом белого.

**Что изменить:** после строки `esp_isp_enable(isp_hdl)` (примерно строка 455–456)
добавить инициализацию AWB и AE контроллеров:

```c
// ── AWB: автобаланс белого ──────────────────────────────────────────────────
esp_isp_awb_config_t awb_cfg = {
    .sample_point = ISP_AWB_SAMPLE_POINT_AFTER_DEMOSAIC,
};
esp_isp_awb_controller_handle_t awb_ctlr = NULL;
esp_err_t awb_ret = esp_isp_new_awb_controller(isp_hdl, &awb_cfg, &awb_ctlr);
if (awb_ret == ESP_OK) {
    esp_isp_awb_controller_enable(awb_ctlr);
    esp_isp_awb_controller_start_continuous_statistics(awb_ctlr);
    ESP_LOGI(TAG, "ISP AWB controller started");
} else {
    ESP_LOGW(TAG, "ISP AWB init failed (%s) — continuing without AWB", esp_err_to_name(awb_ret));
}

// ── AE: автоэкспозиция ─────────────────────────────────────────────────────
esp_isp_ae_config_t ae_cfg = {
    .sample_point = ISP_AE_SAMPLE_POINT_AFTER_DEMOSAIC,
};
esp_isp_ae_controller_handle_t ae_ctlr = NULL;
esp_err_t ae_ret = esp_isp_new_ae_controller(isp_hdl, &ae_cfg, &ae_ctlr);
if (ae_ret == ESP_OK) {
    esp_isp_ae_controller_enable(ae_ctlr);
    esp_isp_ae_controller_start_continuous_statistics(ae_ctlr);
    ESP_LOGI(TAG, "ISP AE controller started");
} else {
    ESP_LOGW(TAG, "ISP AE init failed (%s) — continuing without AE", esp_err_to_name(ae_ret));
}
```

Ошибки инициализации AWB/AE не должны прерывать запуск — поэтому используем
`ESP_LOGW` вместо `ESP_RETURN_ON_ERROR`.

**Необходимые заголовки** (добавить в `#include` секцию если отсутствуют):
```c
#include "esp_isp_awb.h"
#include "esp_isp_ae.h"
```

**Ожидаемый результат:** картинка получит нормальную цветопередачу и яркость.
Это наиболее заметное визуальное улучшение.

---

## Задача 4 — Уменьшить GOP с enc_fps до фиксированного значения 15

**Приоритет:** средний | **Риск:** низкий

**Проблема:** текущий GOP = enc_fps = 50 (строка 490):

```c
.gop = enc_fps,   // = 50 при 50fps сенсора
```

При GOP=50 IDR-кадр вставляется раз в секунду. При любом сетевом событии
(переподключение, потеря пакетов) декодер ждёт до 1 секунды до следующего IDR,
что даёт зависшую картинку. Для FPV важнее частое восстановление.

**Что изменить** в `esp_h264_enc_cfg_hw_t enc_cfg` (строки 488–494):

```c
// Было:
.gop = enc_fps,

// Стало:
.gop = 15,   // IDR каждые ~0.5 сек при 30fps; быстрее восстановление после потерь
```

**Ожидаемый результат:** при сетевых помехах или переподключении картинка
восстанавливается за ≤500ms вместо ≤1000ms. Незначительный рост трафика (~3–5%).

---

## Задача 5 — Скорректировать диапазон QP энкодера

**Приоритет:** средний | **Риск:** низкий

**Проблема:** текущий `qp_max = 45` (строка 493) слишком высокий — при
быстрых движениях энкодер использует максимальное сжатие, что даёт заметные
блочные артефакты. Для FPV-применения важнее стабильное качество при движении,
чем минимальный трафик в статике.

**Что изменить** в `esp_h264_enc_cfg_hw_t enc_cfg`:

```c
// Было:
.rc = { .bitrate = bitrate, .qp_min = 25, .qp_max = 45 },

// Стало:
.rc = { .bitrate = bitrate, .qp_min = 20, .qp_max = 38 },
```

- `qp_min = 20` (было 25): лучшее качество в статичных сценах
- `qp_max = 38` (было 45): меньше артефактов при быстром движении

**Ожидаемый результат:** снижение блочных артефактов при движении камеры.
Возможен рост среднего битрейта на 10–20% при динамичных сценах.

---

## Задача 6 — Добавить логирование encode_ms для P-кадров

**Приоритет:** низкий | **Риск:** нулевой (только логи)

**Проблема:** сейчас `encode_ms` логируется только для первого кадра (строки 192–196).
Без статистики по P-кадрам невозможно подтвердить что encode_ms стабилен
и не является причиной пропуска кадров.

**Что изменить:** расширить блок логирования encode_ms (после строки 196):

```c
// Добавить счётчик и накопитель после объявления first_frame:
static uint32_t s_enc_sum_ms = 0;
static uint32_t s_enc_count  = 0;

// Заменить блок if (first_frame) { ... } на:
if (first_frame) {
    ESP_LOGI(TAG, "First H.264 frame: %"PRIu32" bytes, encode=%"PRIu32" ms",
             out_frame.length, encode_ms);
    first_frame = false;
}
s_enc_sum_ms += encode_ms;
s_enc_count++;
// Статистика каждые 150 кадров (≈5 сек при 30fps)
if (s_enc_count % 150 == 0) {
    uint32_t avg_ms = s_enc_sum_ms / s_enc_count;
    ESP_LOGI(TAG, "H.264 encode avg=%"PRIu32"ms over %"PRIu32" frames",
             avg_ms, s_enc_count);
    s_enc_sum_ms = 0;
    s_enc_count  = 0;
}
```

**Ожидаемый результат:** в логе появятся строки вида
`H.264 encode avg=24ms over 150 frames`, позволяющие точно установить
является ли время кодирования ограничивающим фактором FPS.

---

## Дополнительные улучшения (реализованы поверх задач 1–6)

### Задача 7 — Rate control: fps 50 → 15

**Проблема:** HW энкодер тратит ~81ms на кадр (потолок ~12 fps), но RC настроен на fps=50.
RC выделял 22 Kbit/кадр (P-кадры 2–4 KB) — картинка голодала по битрейту.

**Исправление:** `enc_fps = 15` (соответствует реальному throughput), базовый битрейт 2 → 4 Mbps.
При 800×640 итоговый битрейт ~2.2 Mbps, P-кадры выросли до 6–44 KB.

### Задача 8 — ISP Color: контраст + насыщенность

Контраст 1.5, насыщенность 2.0, яркость -15. Устраняет серый/выцветший вид RAW-картинки.

### Задача 9 — ISP Gamma 1.3

Мягкая gamma-коррекция для подъёма теней без пересвета.
RAW-сенсор выдаёт линейные данные — без gamma картинка плоская.

### Задача 10 — ISP Sharpen (усиленный)

h_freq_coeff=2.0, m_freq_coeff=1.5, пороги h=30/l=5.
Подчёркивает края без нагрузки на энкодер (обработка на ISP).

### Задача 11 — AWB callback

Зарегистрирована ISR-функция `s_awb_stats_done` (IRAM) — устраняет ошибку
`on_statistics_done callback not registered` при `start_continuous_statistics`.

---

## Результаты (из логов)

```
До:
  Сенсор:       OV5647, 800×640@50fps RAW8
  Реальный FPS: ~12 fps (encode avg=81ms)
  P-кадры:      2–4 KB (битрейт-голод)
  Цвет:         зеленоватый, серый, без контраста
  PLI-шторм:    5+ IDR за 2 сек

После:
  Реальный FPS: ~10.5 fps (стриминг) / ~12 fps (без стриминга)
  P-кадры:      6–44 KB (адекватный битрейт)
  Цвет:         нормальный AWB, насыщенный (saturation=2.0)
  Контраст:     ISP Color 1.5 + Gamma 1.3
  Резкость:     ISP Sharpen h=2.0/m=1.5
  PLI:          rate-limited 1.5 сек
  GOP:          15 (восстановление ~1 сек при 10 fps)
```

## Аппаратные ограничения

- **Encode time ~81ms** — потолок HW VEU на ESP32-P4 для 800×640
- **Разрешение 800×640** — лучший RAW8 режим OV5647 (score-based selection)
- **FPS ~10.5 при стриминге** — RTP-пакетизация + SRTP крупных кадров добавляет ~15ms
- **ISP AWB/AE** — контроллеры собирают статистику; хардвер применяет коррекции
  при включённом continuous mode с зарегистрированным callback
