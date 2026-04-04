/**
 * FPV WebRTC Signaling Server
 * Запускается на ноутбуке, Quest 2 подключается через браузер.
 * Протокол: HTTP для статики + WebSocket для SDP/ICE обмена.
 *
 * Использование:
 *   node server.js [--port 8080] [--host 0.0.0.0]
 */

const http = require('http');
const https = require('https');
const fs = require('fs');
const path = require('path');
const { WebSocketServer } = require('ws');

// ─── Параметры ───────────────────────────────────────────────────────────────
const PORT = process.env.PORT || 8080;
const HOST = process.env.HOST || '0.0.0.0';
const USE_TLS = process.env.TLS === '1'; // WebXR требует HTTPS или localhost

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js':   'application/javascript',
  '.css':  'text/css',
  '.json': 'application/json',
  '.ico':  'image/x-icon',
};

// ─── HTTP сервер (отдаёт статику из ../public) ─────────────────────────────
function createServer() {
  if (USE_TLS) {
    const certDir = path.join(__dirname, 'certs');
    let key, cert;
    try {
      key  = fs.readFileSync(path.join(certDir, 'key.pem'));
      cert = fs.readFileSync(path.join(certDir, 'cert.pem'));
    } catch {
      console.error('Сертификаты не найдены. Запустите: ./gen-certs.sh');
      process.exit(1);
    }
    return https.createServer({ key, cert }, handleRequest);
  }
  return http.createServer(handleRequest);
}

function handleRequest(req, res) {
  let filePath = path.join(__dirname, '..', 'public',
    req.url === '/' ? 'index.html' : req.url);

  // Защита от path traversal
  const publicDir = path.resolve(path.join(__dirname, '..', 'public'));
  if (!path.resolve(filePath).startsWith(publicDir)) {
    res.writeHead(403); res.end('Forbidden'); return;
  }

  fs.readFile(filePath, (err, data) => {
    if (err) {
      res.writeHead(404, { 'Content-Type': 'text/plain' });
      res.end('404 Not Found: ' + req.url);
      return;
    }
    const ext = path.extname(filePath);
    res.writeHead(200, {
      'Content-Type': MIME[ext] || 'application/octet-stream',
      'Cache-Control': 'no-cache',
      // Разрешаем WebXR + SharedArrayBuffer
      'Cross-Origin-Embedder-Policy': 'require-corp',
      'Cross-Origin-Opener-Policy':   'same-origin',
    });
    res.end(data);
  });
}

// ─── WebSocket сигналинг ───────────────────────────────────────────────────
// Простая модель: два пира — "streamer" (ноутбук/ESP32) и "viewer" (Quest 2)
// Сообщения: { type: 'offer'|'answer'|'ice'|'role', ... }

const server = createServer();
const wss    = new WebSocketServer({ server });

let peers = {};          // { streamer: ws, viewer: ws }
let iceBufs = {};        // буфер ICE-кандидатов пока второй пир не готов

wss.on('connection', (ws, req) => {
  const ip = req.socket.remoteAddress;
  console.log(`[WS] connect  ${ip}`);
  let role = null;

  ws.on('message', (raw) => {
    let msg;
    try { msg = JSON.parse(raw); } catch { return; }

    // ── Регистрация роли ────────────────────────────────────────────────────
    if (msg.type === 'role') {
      const claimed = msg.role;
      if (claimed !== 'streamer' && claimed !== 'viewer') {
        console.warn(`[WS] ${ip} sent invalid role: ${claimed}`);
        ws.close(1008, 'invalid role');
        return;
      }
      role = claimed;
      peers[role] = ws;
      iceBufs[role] = [];
      console.log(`[WS] ${ip} registered as ${role}`);

      // Если оба пира уже есть — сообщаем стримеру создать offer
      if (peers.streamer && peers.viewer) {
        safeSend(peers.streamer, { type: 'viewer_ready' });
      }
      return;
    }

    // ── Проброс SDP offer / answer ──────────────────────────────────────────
    if (msg.type === 'offer' && peers.viewer) {
      safeSend(peers.viewer, msg);
      // Отправить накопившиеся ICE кандидаты стримера вьюверу
      (iceBufs.streamer || []).forEach(c => safeSend(peers.viewer, c));
      iceBufs.streamer = [];
    }

    if (msg.type === 'answer' && peers.streamer) {
      safeSend(peers.streamer, msg);
      (iceBufs.viewer || []).forEach(c => safeSend(peers.streamer, c));
      iceBufs.viewer = [];
    }

    // ── ICE кандидаты ───────────────────────────────────────────────────────
    if (msg.type === 'ice') {
      const target = (role === 'streamer') ? 'viewer' : 'streamer';
      if (peers[target] && peers[target].readyState === 1 /* OPEN */) {
        safeSend(peers[target], msg);
      } else {
        (iceBufs[role] = iceBufs[role] || []).push(msg);
      }
    }
  });

  ws.on('close', () => {
    console.log(`[WS] disconnect ${ip} (${role})`);
    if (role) {
      delete peers[role];
      delete iceBufs[role];
      // Уведомить другого пира
      const other = role === 'streamer' ? 'viewer' : 'streamer';
      if (peers[other]) safeSend(peers[other], { type: 'peer_disconnected' });
    }
  });

  ws.on('error', (e) => console.error('[WS] error', e.message));
});

function safeSend(ws, obj) {
  if (ws && ws.readyState === 1) {
    ws.send(JSON.stringify(obj));
  }
}

// ─── Запуск ────────────────────────────────────────────────────────────────
server.listen(PORT, HOST, () => {
  const proto = USE_TLS ? 'https' : 'http';
  console.log(`\n✓ FPV Signaling Server запущен`);
  console.log(`  Локально:  ${proto}://localhost:${PORT}`);
  console.log(`  В сети:    ${proto}://<IP_ноутбука>:${PORT}`);
  console.log(`\n  Quest 2: откройте браузер → ${proto}://<IP_ноутбука>:${PORT}`);
  console.log(`  Для получения IP: ipconfig (Windows) / ip a (Linux)\n`);
});
