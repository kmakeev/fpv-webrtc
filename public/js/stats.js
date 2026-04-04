/**
 * stats.js
 * Опрашивает RTCPeerConnection.getStats() каждую секунду.
 * Отображает: задержку, FPS, разрешение, кодек.
 * Экспортирует: window.FPVStats
 */
(function () {
  'use strict';

  let _pc       = null;
  let _timer    = null;
  let _prevBytes = 0;
  let _prevTs   = 0;
  let _prevFrames = 0;

  const els = {
    latency:    document.getElementById('stat-latency'),
    fps:        document.getElementById('stat-fps'),
    resolution: document.getElementById('stat-resolution'),
    codec:      document.getElementById('stat-codec'),
  };

  function _fmt(el, label, value) {
    if (el) el.textContent = value ? `${label}: ${value}` : '—';
  }

  async function _poll() {
    if (!_pc) return;
    try {
      const stats = await _pc.getStats();
      let inboundVideo = null;
      let candidatePair = null;

      stats.forEach(report => {
        if (report.type === 'inbound-rtp' && report.kind === 'video') {
          inboundVideo = report;
        }
        if (report.type === 'candidate-pair' && report.state === 'succeeded') {
          candidatePair = report;
        }
      });

      // Задержка (RTT / 2 как приближение)
      if (candidatePair?.currentRoundTripTime != null) {
        const ms = Math.round(candidatePair.currentRoundTripTime * 500);
        _fmt(els.latency, 'Latency', `~${ms} ms`);
      }

      // FPS
      if (inboundVideo) {
        const frames = inboundVideo.framesDecoded || 0;
        const now    = inboundVideo.timestamp;
        if (_prevFrames > 0 && _prevTs > 0) {
          const dt  = (now - _prevTs) / 1000;
          const fps = Math.round((frames - _prevFrames) / dt);
          _fmt(els.fps, 'FPS', fps);
        }
        _prevFrames = frames;
        _prevTs     = inboundVideo.timestamp;

        // Разрешение
        const w = inboundVideo.frameWidth;
        const h = inboundVideo.frameHeight;
        if (w && h) _fmt(els.resolution, 'Res', `${w}×${h}`);

        // Кодек
        if (inboundVideo.codecId) {
          const codecReport = stats.get(inboundVideo.codecId);
          if (codecReport?.mimeType) {
            _fmt(els.codec, 'Codec', codecReport.mimeType.replace('video/', ''));
          }
        }
      }
    } catch (e) {
      // Соединение могло закрыться
    }
  }

  const FPVStats = {
    start(pc) {
      _pc = pc;
      _prevBytes = _prevFrames = _prevTs = 0;
      _timer = setInterval(_poll, 1000);
    },
    stop() {
      clearInterval(_timer);
      _pc = null;
      if (els.latency)    els.latency.textContent    = '—';
      if (els.fps)        els.fps.textContent        = '—';
      if (els.resolution) els.resolution.textContent = '—';
      if (els.codec)      els.codec.textContent      = '—';
    },
  };

  window.FPVStats = FPVStats;
})();
