/**
 * stats.js
 * Опрашивает RTCPeerConnection.getStats() каждую секунду.
 *
 * Метрики задержки:
 *   Сеть    — RTT/2 из candidate-pair (одностороннее сетевое время)
 *   Декод   — среднее время декодирования кадра (totalDecodeTime / framesDecoded)
 *   Буфер   — задержка джиттер-буфера (jitterBufferDelay / jitterBufferEmittedCount)
 *   Итого   — сумма выше: оценка задержки на стороне получателя
 *             (не включает захват и энкодинг на стороне стримера)
 *
 * Экспортирует: window.FPVStats
 */
(function () {
  'use strict';

  let _pc         = null;
  let _timer      = null;
  let _prevTs     = 0;
  let _prevFrames = 0;
  let _prevDecodeTime = 0;
  let _prevDecodeFrames = 0;

  // Элементы основного статус-бара (под кнопками)
  const bar = {
    latency:    document.getElementById('stat-latency'),
    fps:        document.getElementById('stat-fps'),
    resolution: document.getElementById('stat-resolution'),
    codec:      document.getElementById('stat-codec'),
  };

  // Элементы VR HUD (dom-overlay, не используется — оставлено для совместимости)
  const hud = {
    net:    document.getElementById('vr-net'),
    decode: document.getElementById('vr-decode'),
    jitter: document.getElementById('vr-jitter'),
    total:  document.getElementById('vr-total'),
    fps:    document.getElementById('vr-fps'),
    res:    document.getElementById('vr-res'),
    codec:  document.getElementById('vr-codec'),
  };

  // Элементы flat-stats (поверх плоского видео)
  // Элементы создаются динамически в main.js, поэтому ищем через геттер
  function fs(id) { return document.getElementById(id); }

  function _set(el, text) { if (el) el.textContent = text; }

  function _reset() {
    _set(bar.latency,    '—');
    _set(bar.fps,        '—');
    _set(bar.resolution, '—');
    _set(bar.codec,      '—');
    _set(hud.net,    'Сеть: —');
    _set(hud.decode, 'Декод: —');
    _set(hud.jitter, 'Буфер: —');
    _set(hud.total,  'Итого: —');
    _set(hud.fps,    '—');
    _set(hud.res,    '—');
    _set(hud.codec,  '—');
  }

  async function _poll() {
    if (!_pc) return;
    try {
      const stats = await _pc.getStats();
      let inbound     = null;
      let candPair    = null;

      stats.forEach(r => {
        if (r.type === 'inbound-rtp'   && r.kind === 'video') inbound  = r;
        if (r.type === 'candidate-pair' && r.state === 'succeeded')    candPair = r;
      });

      // ── Сетевая задержка (RTT / 2) ──────────────────────────────────────
      let netMs = null;
      if (candPair?.currentRoundTripTime != null) {
        netMs = Math.round(candPair.currentRoundTripTime * 500);
      }

      // ── Время декодирования (разница между соседними замерами) ──────────
      let decodeMs = null;
      if (inbound?.totalDecodeTime != null && inbound?.framesDecoded > 0) {
        const dTime   = inbound.totalDecodeTime  - _prevDecodeTime;
        const dFrames = inbound.framesDecoded    - _prevDecodeFrames;
        if (dFrames > 0) {
          decodeMs = Math.round((dTime / dFrames) * 1000);
        }
        _prevDecodeTime   = inbound.totalDecodeTime;
        _prevDecodeFrames = inbound.framesDecoded;
      }

      // ── Задержка джиттер-буфера ─────────────────────────────────────────
      let jitterMs = null;
      if (inbound?.jitterBufferDelay != null && inbound?.jitterBufferEmittedCount > 0) {
        jitterMs = Math.round(
          (inbound.jitterBufferDelay / inbound.jitterBufferEmittedCount) * 1000
        );
      }

      // ── Итоговая оценка (всё что измеримо на приёмнике) ─────────────────
      const totalMs = (netMs ?? 0) + (decodeMs ?? 0) + (jitterMs ?? 0);

      // ── FPS ─────────────────────────────────────────────────────────────
      let fps = null;
      if (inbound) {
        const frames = inbound.framesDecoded || 0;
        const now    = inbound.timestamp;
        if (_prevFrames > 0 && _prevTs > 0) {
          const dt = (now - _prevTs) / 1000;
          fps = Math.round((frames - _prevFrames) / dt);
        }
        _prevFrames = frames;
        _prevTs     = inbound.timestamp;
      }

      // ── Разрешение ───────────────────────────────────────────────────────
      const w = inbound?.frameWidth;
      const h = inbound?.frameHeight;
      const resStr = (w && h) ? `${w}×${h}` : null;

      // ── Кодек ────────────────────────────────────────────────────────────
      let codecStr = null;
      if (inbound?.codecId) {
        const cr = stats.get(inbound.codecId);
        if (cr?.mimeType) codecStr = cr.mimeType.replace('video/', '');
      }

      // ── Запись в основной бар ─────────────────────────────────────────
      _set(bar.latency,    netMs    != null ? `Net: ~${netMs} ms`   : '—');
      _set(bar.fps,        fps      != null ? `${fps} fps`          : '—');
      _set(bar.resolution, resStr              ? resStr               : '—');
      _set(bar.codec,      codecStr            ? codecStr             : '—');

      // ── Запись в DOM VR HUD ──────────────────────────────────────────────
      _set(hud.net,    netMs    != null ? `Сеть: ~${netMs}ms`       : 'Сеть: —');
      _set(hud.decode, decodeMs != null ? `Декод: ${decodeMs}ms`    : 'Декод: —');
      _set(hud.jitter, jitterMs != null ? `Буфер: ${jitterMs}ms`    : 'Буфер: —');
      _set(hud.total,  totalMs  >  0    ? `Итого: ~${totalMs}ms`    : 'Итого: —');
      _set(hud.fps,    fps      != null ? `${fps} fps`              : '—');
      _set(hud.res,    resStr              ? resStr                   : '—');
      _set(hud.codec,  codecStr            ? codecStr                 : '—');

      // ── Flat stats overlay ───────────────────────────────────────────────
      _set(fs('fs-net'),    netMs    != null ? `Сеть: ~${netMs}ms`    : 'Сеть: —');
      _set(fs('fs-decode'), decodeMs != null ? `Декод: ${decodeMs}ms` : 'Декод: —');
      _set(fs('fs-jitter'), jitterMs != null ? `Буфер: ${jitterMs}ms` : 'Буфер: —');
      _set(fs('fs-total'),  totalMs  >  0    ? `Итого: ~${totalMs}ms` : 'Итого: —');
      _set(fs('fs-fps'),    fps      != null ? `${fps} fps`           : '—');
      _set(fs('fs-res'),    resStr           ? resStr                  : '—');
      _set(fs('fs-codec'),  codecStr         ? codecStr                : '—');

      // ── WebGL HUD (рендерится прямо в XR-сцену) ─────────────────────────
      if (window.FPVRenderer) {
        const net  = netMs    != null ? `~${netMs}ms`    : '—ms';
        const dec  = decodeMs != null ? `${decodeMs}ms`  : '—ms';
        const buf  = jitterMs != null ? `${jitterMs}ms`  : '—ms';
        const tot  = totalMs  >  0    ? `~${totalMs}ms`  : '—ms';
        const line1 = `Сеть ${net}  Декод ${dec}  Буфер ${buf}  Итого ${tot}`;
        const line2 = `${fps ?? '—'} fps  ${resStr ?? '—'}  ${codecStr ?? '—'}`;
        FPVRenderer.updateStats(line1, line2);
      }

    } catch (_e) {
      // соединение могло закрыться
    }
  }

  const FPVStats = {
    start(pc) {
      _pc = pc;
      _prevTs = _prevFrames = _prevDecodeTime = _prevDecodeFrames = 0;
      _timer = setInterval(_poll, 1000);
    },
    stop() {
      clearInterval(_timer);
      _pc = null;
      _reset();
    },
  };

  window.FPVStats = FPVStats;
})();
