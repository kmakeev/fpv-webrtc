/**
 * webrtc-client.js
 * Управляет WebRTC соединением: сигналинг через WS, получает видеопоток.
 * Экспортирует глобальный объект window.FPVClient.
 */
(function () {
  'use strict';

  // ── Конфигурация ICE ──────────────────────────────────────────────────────
  // В локальной сети STUN не нужен, но оставим Google STUN для надёжности
  const ICE_CONFIG = {
    iceServers: [
      { urls: 'stun:stun.l.google.com:19302' },
    ],
    iceTransportPolicy: 'all',
    bundlePolicy: 'max-bundle',
    rtcpMuxPolicy: 'require',
  };

  // ── Состояние ─────────────────────────────────────────────────────────────
  let ws          = null;
  let pc          = null;          // RTCPeerConnection
  let wsUrl       = null;
  let reconnTimer = null;
  let stream      = null;          // входящий MediaStream

  const RECONNECT_DELAY = 3000;

  // ── Публичный API ─────────────────────────────────────────────────────────
  const FPVClient = {
    /** Запустить подключение к серверу */
    connect(serverUrl) {
      wsUrl = serverUrl;
      _connectWs();
    },

    /** Получить входящий MediaStream (видео с ESP32 / ноутбука) */
    getStream() { return stream; },

    /** Получить RTCPeerConnection для статистики */
    getPeerConnection() { return pc; },

    /** Колбэки — задаются снаружи */
    onStatus:     () => {},   // (status: string, detail?: string) => void
    onStream:     () => {},   // (stream: MediaStream) => void
    onDisconnect: () => {},
  };

  // ── WebSocket ─────────────────────────────────────────────────────────────
  function _connectWs() {
    clearTimeout(reconnTimer);
    const url = wsUrl.replace(/^http/, 'ws');
    FPVClient.onStatus('connecting', 'WebSocket → ' + url);

    ws = new WebSocket(url);

    ws.onopen = () => {
      console.log('[WS] connected');
      // Регистрируемся как viewer
      _send({ type: 'role', role: 'viewer' });
      FPVClient.onStatus('waiting', 'Ожидание стримера...');
      _createPeerConnection();
    };

    ws.onmessage = (ev) => {
      let msg;
      try { msg = JSON.parse(ev.data); } catch { return; }
      _handleSignal(msg);
    };

    ws.onclose = (ev) => {
      console.log('[WS] closed', ev.code);
      FPVClient.onStatus('disconnected', 'WS закрыт, переподключение...');
      FPVClient.onDisconnect();
      _cleanup();
      reconnTimer = setTimeout(_connectWs, RECONNECT_DELAY);
    };

    ws.onerror = (e) => {
      console.error('[WS] error', e);
    };
  }

  function _send(obj) {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify(obj));
    }
  }

  // ── RTCPeerConnection ─────────────────────────────────────────────────────
  function _createPeerConnection() {
    if (pc) { pc.close(); }

    pc = new RTCPeerConnection(ICE_CONFIG);

    // Получение входящих треков (видео от ESP32-P4 / ноутбука)
    pc.ontrack = (ev) => {
      console.log('[PC] track received:', ev.track.kind);
      if (ev.streams && ev.streams[0]) {
        stream = ev.streams[0];
        FPVClient.onStatus('streaming', 'Поток активен');
        FPVClient.onStream(stream);
      }
    };

    // ICE кандидаты → отправить на сервер
    pc.onicecandidate = (ev) => {
      if (ev.candidate) {
        _send({ type: 'ice', candidate: ev.candidate });
      }
    };

    pc.oniceconnectionstatechange = () => {
      const s = pc.iceConnectionState;
      console.log('[PC] ICE:', s);
      if (s === 'disconnected' || s === 'failed' || s === 'closed') {
        FPVClient.onStatus('error', 'ICE: ' + s);
      }
    };

    pc.onconnectionstatechange = () => {
      const s = pc.connectionState;
      console.log('[PC] connection:', s);
      if (s === 'connected') {
        FPVClient.onStatus('streaming', 'Соединение установлено');
      }
    };
  }

  // ── Обработка сигналинг-сообщений ────────────────────────────────────────
  async function _handleSignal(msg) {
    if (!pc) return;

    switch (msg.type) {

      // Стример прислал offer
      case 'offer':
        if (!msg.sdp) { console.warn('[PC] received offer without sdp'); return; }
        try {
          await pc.setRemoteDescription(new RTCSessionDescription(msg));
          const answer = await pc.createAnswer();
          // Предпочесть H.264 (нужно для аппаратного декодирования на Quest)
          answer.sdp = _preferH264(answer.sdp);
          await pc.setLocalDescription(answer);
          _send({ type: 'answer', sdp: answer.sdp, type: answer.type });
          console.log('[PC] answer sent');
        } catch (e) {
          console.error('[PC] answer error:', e);
          FPVClient.onStatus('error', e.message);
        }
        break;

      // ICE кандидат от стримера
      case 'ice':
        try {
          if (msg.candidate) {
            await pc.addIceCandidate(new RTCIceCandidate(msg.candidate));
          }
        } catch (e) {
          console.warn('[PC] addIceCandidate:', e.message);
        }
        break;

      case 'peer_disconnected':
        FPVClient.onStatus('waiting', 'Стример отключился...');
        stream = null;
        FPVClient.onDisconnect();
        break;
    }
  }

  // ── Предпочесть H.264 в SDP ───────────────────────────────────────────────
  // Quest 2 поддерживает аппаратное декодирование H.264 через MediaCodec.
  // Переставляем H.264 на первое место в списке кодеков.
  function _preferH264(sdp) {
    return sdp.split('\r\n').reduce((lines, line) => {
      if (!line.startsWith('m=video')) { lines.push(line); return lines; }

      // Найти payload types для H264
      const parts  = line.split(' ');
      const header = parts.slice(0, 3);   // m=video <port> RTP/SAVPF
      const pts    = parts.slice(3);      // список PT

      // Найти PT строк с H264 в следующих rtpmap/fmtp
      const h264pts = [];
      const others  = [];
      pts.forEach(pt => {
        // Ищем в последующих строках rtpmap для этого PT
        const rtpmap = sdp.split('\r\n')
          .find(l => l.startsWith(`a=rtpmap:${pt} H264`));
        if (rtpmap) h264pts.push(pt); else others.push(pt);
      });

      lines.push([...header, ...h264pts, ...others].join(' '));
      return lines;
    }, []).join('\r\n');
  }

  // ── Очистка ───────────────────────────────────────────────────────────────
  function _cleanup() {
    if (pc) { try { pc.close(); } catch {} pc = null; }
    stream = null;
  }

  window.FPVClient = FPVClient;
})();
