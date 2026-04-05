/**
 * datachannel.js
 * Общий модуль WebRTC DataChannel для стримера и вьювера.
 *
 * Стример:  FPVDataChannel.create(pc)  — создаёт канал 'fpv'
 * Вьювер:   FPVDataChannel.accept(pc)  — принимает канал через ondatachannel
 *
 * Синхронизация часов (NTP-подобная, 5 round-trips, вьювер инициирует):
 *   clockOffset = streamer_clock - viewer_clock (мс)
 *   e2e = Date.now() - captureTimestamp - clockOffset
 *
 * Типы сообщений:
 *   ping  { type, id, t0 }              вьювер → стример
 *   pong  { type, id, t0, t1 }          стример → вьювер  (t1 = время получения пинга)
 *   ts    { type, capture, encode }     стример → вьювер  (раз в секунду)
 *   head  { type, x, y, z, w }         вьювер → стример  (поза головы, будущее)
 *
 * Экспортирует: window.FPVDataChannel
 */
(function () {
  'use strict';

  let _dc          = null;
  let _clockOffset = null;   // смещение часов стримера относительно вьювера (мс)
  let _gen         = 0;      // поколение DC — защита от устаревших колбэков
  let _pendingPings = {};    // id → { resolve, reject }

  // ── Внутренние функции ────────────────────────────────────────────────────

  function _dcSend(obj) {
    if (_dc && _dc.readyState === 'open') {
      _dc.send(JSON.stringify(obj));
    }
  }

  function _onOpen() {
    console.log('[DC] open');
    if (FPVDataChannel.onOpen) FPVDataChannel.onOpen();
  }

  function _onClose() {
    console.log('[DC] closed');
    _clockOffset = null;
    // Отменяем все ожидающие пинги
    const pending = _pendingPings;
    _pendingPings = {};
    Object.values(pending).forEach(p => p.reject(new Error('DC closed')));
    if (FPVDataChannel.onClose) FPVDataChannel.onClose();
  }

  function _onMessage(ev) {
    let msg;
    try { msg = JSON.parse(ev.data); } catch { return; }

    switch (msg.type) {

      // Вьювер прислал пинг — отвечаем понгом (стример)
      case 'ping':
        _dcSend({ type: 'pong', id: msg.id, t0: msg.t0, t1: Date.now() });
        break;

      // Стример прислал понг — разрешаем ожидающий промис (вьювер)
      case 'pong': {
        const p = _pendingPings[msg.id];
        if (p) {
          delete _pendingPings[msg.id];
          p.resolve({ t0: msg.t0, t1: msg.t1, t2: Date.now() });
        }
        break;
      }

      // Стример прислал временну́ю метку захвата (вьювер)
      case 'ts':
        if (FPVDataChannel.onTimestamp) {
          FPVDataChannel.onTimestamp({ capture: msg.capture, encode: msg.encode });
        }
        break;

      // Поза головы (будущее использование на стороне стримера)
      case 'head':
        if (FPVDataChannel.onHeadPose) {
          FPVDataChannel.onHeadPose(msg);
        }
        break;
    }
  }

  function _attachDc(dc) {
    const myGen = ++_gen;
    _dc = dc;
    _dc.onopen    = () => { if (_gen === myGen) _onOpen(); };
    _dc.onclose   = () => { if (_gen === myGen) _onClose(); };
    _dc.onmessage = (ev) => { if (_gen === myGen) _onMessage(ev); };
    _dc.onerror   = (e) => {
      if (_gen !== myGen) return;
      // 'User-Initiated Abort' — нормальное закрытие при pc.close(), не логируем как ошибку
      const msg = e.error?.message || '';
      if (msg.includes('User-Initiated Abort') || msg.includes('Close called')) return;
      console.error('[DC] error:', e);
    };
    // Некоторые браузеры (Quest) стреляют ondatachannel когда канал уже open —
    // в этом случае onopen никогда не придёт, вызываем вручную.
    if (dc.readyState === 'open') {
      setTimeout(() => { if (_gen === myGen) _onOpen(); }, 0);
    }
  }

  // ── Вспомогательные утилиты ────────────────────────────────────────────────

  function _median(arr) {
    const sorted = [...arr].sort((a, b) => a - b);
    const mid = Math.floor(sorted.length / 2);
    return sorted.length % 2 !== 0 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2;
  }

  function _ping(id) {
    return new Promise((resolve, reject) => {
      _pendingPings[id] = { resolve, reject };
      _dcSend({ type: 'ping', id, t0: Date.now() });
    });
  }

  // ── Публичный API ──────────────────────────────────────────────────────────
  const FPVDataChannel = {

    /**
     * Стример: создать DataChannel на готовом RTCPeerConnection.
     * Вызывать до createOffer() — чтобы DC попал в SDP.
     */
    create(pc) {
      const dc = pc.createDataChannel('fpv', { ordered: true });
      _attachDc(dc);
      return dc;
    },

    /**
     * Вьювер: подписаться на входящий DataChannel от стримера.
     * Вызывать сразу после создания RTCPeerConnection.
     */
    accept(pc) {
      pc.ondatachannel = (ev) => {
        _attachDc(ev.channel);
      };
    },

    /**
     * Вьювер: выполнить NTP-подобную синхронизацию часов.
     * 5 round-trips → медиана → clockOffset.
     * Должна вызываться после onOpen.
     * @returns {Promise<number|null>} смещение часов в мс или null при ошибке
     */
    async syncClocks() {
      const offsets = [];
      for (let i = 0; i < 5; i++) {
        try {
          const result = await Promise.race([
            _ping(i),
            new Promise((_, reject) => setTimeout(() => reject(new Error('ping timeout')), 2000)),
          ]);
          const { t0, t1, t2 } = result;
          const rtt = t2 - t0;
          offsets.push(t1 - t0 - rtt / 2);
          // Небольшая пауза между пингами
          if (i < 4) await new Promise(r => setTimeout(r, 50));
        } catch (e) {
          console.warn('[DC] ping', i, 'failed:', e.message);
        }
      }

      if (offsets.length === 0) {
        console.warn('[DC] clock sync failed — no responses');
        return null;
      }

      _clockOffset = _median(offsets);
      console.log(`[DC] clock offset: ${Math.round(_clockOffset)} ms (${offsets.length}/5 samples)`);
      if (FPVDataChannel.onClockSynced) FPVDataChannel.onClockSynced(_clockOffset);
      return _clockOffset;
    },

    /**
     * Отправить JSON-объект через DataChannel (если канал открыт).
     * Используется стримером для отправки ts-сообщений и вьювером для head-сообщений.
     */
    send(obj) {
      _dcSend(obj);
    },

    /** Текущее смещение часов (мс). null — синхронизация не выполнена. */
    getOffset() { return _clockOffset; },

    // ── Колбэки (задаются снаружи) ─────────────────────────────────────────
    onOpen:        null,   // () => void
    onClose:       null,   // () => void
    onClockSynced: null,   // (offsetMs: number) => void
    onTimestamp:   null,   // ({ capture: number, encode: number }) => void
    onHeadPose:    null,   // ({ x, y, z, w }) => void  (стример)
  };

  window.FPVDataChannel = FPVDataChannel;
})();
