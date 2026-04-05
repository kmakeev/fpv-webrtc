/**
 * main.js
 * Координирует: UI ↔ WebRTC клиент ↔ WebXR рендерер ↔ статистика.
 * Определяет IP сервера автоматически из window.location.
 */
(function () {
  'use strict';

  // ── DOM ──────────────────────────────────────────────────────────────────
  const ui         = document.getElementById('ui');
  const statusEl   = document.getElementById('status');
  const detailsEl  = document.getElementById('details');
  const btnVR      = document.getElementById('btn-vr');
  const btnFlat    = document.getElementById('btn-flat');
  const videoFlat  = document.getElementById('video-flat');
  const xrCanvas   = document.getElementById('xr-canvas');
  const flatStats  = document.getElementById('flat-stats');
  flatStats.innerHTML =
    '<div class="fs-row">' +
      '<span id="fs-net">Сеть: —</span>' +
      '<span>·</span>' +
      '<span id="fs-decode">Декод: —</span>' +
      '<span>·</span>' +
      '<span id="fs-jitter">Буфер: —</span>' +
      '<span>·</span>' +
      '<span id="fs-total">Итого: —</span>' +
    '</div>' +
    '<div class="fs-row fs-row2">' +
      '<span id="fs-fps">—</span>' +
      '<span>·</span>' +
      '<span id="fs-res">—</span>' +
      '<span>·</span>' +
      '<span id="fs-codec">—</span>' +
    '</div>';

  // ── Определяем адрес сервера ──────────────────────────────────────────────
  // Если открыто с ноутбука — ws://localhost:8080
  // Если с Quest 2         — ws://<IP_ноутбука>:8080
  const serverOrigin = window.location.origin;     // http://192.168.x.x:8080
  const wsUrl        = serverOrigin;               // webrtc-client сам заменит http→ws

  // ── Режимы отображения ────────────────────────────────────────────────────
  let mode = 'none';   // 'none' | 'flat' | 'vr'

  // ── Инициализация рендерера ───────────────────────────────────────────────
  let rendererReady = false;
  FPVRenderer.init(xrCanvas)
    .then(() => {
      rendererReady = true;
      console.log('[Main] WebGL renderer ready');
    })
    .catch(e => {
      console.error('[Main] WebGL init failed:', e);
      setStatus('error', 'WebGL2: ' + e.message);
    });

  // ── Колбэки от рендерера ──────────────────────────────────────────────────
  FPVRenderer.onSessionEnd = () => {
    xrCanvas.style.display = 'none';
    ui.classList.remove('hidden');
    mode = 'none';
  };

  FPVRenderer.onHeadPose = (quaternion) => {
    // TODO: отправить в DataChannel для управления сервой камеры
    // Будет реализовано в следующем модуле (datachannel.js)
    // _dc.send(JSON.stringify({ type: 'head', ...quaternion }));
  };

  // ── Статус UI ─────────────────────────────────────────────────────────────
  function setStatus(cls, label, detail) {
    statusEl.className = 'status ' + cls;
    statusEl.textContent = {
      connecting:   '⟳ Подключение...',
      waiting:      '⏳ ' + (label || 'Ожидание стримера'),
      streaming:    '● ' + (label || 'Поток активен'),
      error:        '✗ Ошибка',
      disconnected: '○ Отключено',
    }[cls] || label;
    if (detailsEl) detailsEl.textContent = detail || '';
  }

  // ── WebRTC клиент ─────────────────────────────────────────────────────────
  FPVClient.onStatus = (status, detail) => {
    setStatus(status, null, detail);

    const hasStream = (status === 'streaming');
    btnFlat.disabled = !hasStream;

    // VR-кнопку включаем если есть поток И поддерживается WebXR
    if (hasStream) {
      navigator.xr?.isSessionSupported('immersive-vr')
        .then(ok => { btnVR.disabled = !ok; })
        .catch(() => { btnVR.disabled = true; });
    } else {
      btnVR.disabled = true;
    }
  };

  FPVClient.onStream = (stream) => {
    console.log('[Main] stream received', stream.getTracks());

    // Подключаем к плоскому видео (показывает при нажатии "Смотреть на экране")
    videoFlat.srcObject = stream;
    videoFlat.play().catch(console.warn);

    // Запускаем сбор статистики
    FPVStats.start(FPVClient.getPeerConnection());
  };

  FPVClient.onDisconnect = () => {
    FPVStats.stop();
    videoFlat.srcObject = null;
    flatStats.style.display = 'none';
    if (mode === 'flat') {
      videoFlat.style.display = 'none';
      ui.classList.remove('hidden');
      mode = 'none';
    }
  };

  // ── Кнопка "Войти в VR" ───────────────────────────────────────────────────
  btnVR.addEventListener('click', async () => {
    if (!rendererReady) { setStatus('error', null, 'WebGL не готов'); return; }
    const stream = FPVClient.getStream();
    if (!stream) { setStatus('error', null, 'Нет потока'); return; }

    try {
      // Создаём скрытый video-элемент для текстуры (не videoFlat)
      const vrVideo = document.createElement('video');
      vrVideo.autoplay    = true;
      vrVideo.playsInline = true;
      vrVideo.muted       = true;
      vrVideo.srcObject   = stream;
      // Элемент должен быть в DOM чтобы play() сработал в мобильном браузере
      vrVideo.style.cssText = 'position:fixed;width:1px;height:1px;opacity:0;pointer-events:none';
      document.body.appendChild(vrVideo);
      await vrVideo.play();

      xrCanvas.style.display = 'block';
      ui.classList.add('hidden');
      mode = 'vr';

      await FPVRenderer.startXR(vrVideo);
    } catch (e) {
      console.error('[Main] XR start error:', e);
      xrCanvas.style.display = 'none';
      ui.classList.remove('hidden');
      mode = 'none';
      setStatus('error', null, 'VR: ' + e.message);
      // Убрать временный video из DOM при ошибке
      document.querySelectorAll('video[style*="opacity:0"]').forEach(v => v.remove());
    }
  });

  // ── Кнопка "Смотреть на экране" ───────────────────────────────────────────
  btnFlat.addEventListener('click', () => {
    const stream = FPVClient.getStream();
    if (!stream) return;

    if (mode === 'flat') {
      videoFlat.style.display = 'none';
      flatStats.style.display  = 'none';
      ui.classList.remove('hidden');
      mode = 'none';
      btnFlat.textContent = 'Смотреть на экране';
    } else {
      videoFlat.srcObject = stream;
      videoFlat.style.display = 'block';
      flatStats.style.display  = 'flex';
      ui.classList.add('hidden');
      mode = 'flat';
      btnFlat.textContent = '← Назад';
      // Воспроизведение внутри обработчика клика — гарантирует жест пользователя
      videoFlat.play().catch(e => {
        console.error('[Main] videoFlat.play failed:', e);
        setStatus('error', null, 'Видео: ' + e.message);
      });
    }
  });

  // ── Проверка поддержки WebXR (инфо для пользователя) ─────────────────────
  async function checkWebXR() {
    if (!navigator.xr) {
      detailsEl.textContent = 'WebXR недоступен (нужен браузер Quest)';
      return;
    }
    const ok = await navigator.xr.isSessionSupported('immersive-vr').catch(() => false);
    if (!ok) {
      detailsEl.textContent = 'immersive-vr не поддерживается на этом устройстве';
    }
  }
  checkWebXR();

  // ── Старт ─────────────────────────────────────────────────────────────────
  console.log('[Main] connecting to', wsUrl);
  FPVClient.connect(wsUrl);

})();
