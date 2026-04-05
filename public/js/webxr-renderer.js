/**
 * webxr-renderer.js
 * Рендерит входящий видеопоток в стерео через WebXR Immersive VR.
 *
 * Принцип:
 *  1. Создаём WebGL2 текстуру из <video>
 *  2. В каждом XR-кадре обновляем текстуру и рисуем два полных квада
 *     (для левого и правого глаза) с небольшим IPD-смещением
 *  3. Ориентация головы (reprojection) обрабатывается шлемом автоматически
 *     через ATW (Asynchronous TimeWarp) внутри браузера
 *
 * Экспорт: window.FPVRenderer
 */
(function () {
  'use strict';

  const FPVRenderer = {
    // ── Состояние ────────────────────────────────────────────────────────
    session:     null,
    gl:          null,
    canvas:      null,
    texture:     null,
    program:     null,
    vao:         null,
    xrRefSpace:  null,
    videoEl:     null,
    // Stats overlay (2D canvas → WebGL текстура)
    _statsCanvas: null,
    _statsCtx:    null,
    _statsTex:    null,

    // Настройки отображения
    fov:         90,     // градусов (FOV по горизонтали)
    ipd:         0.064,  // метров (межзрачковое расстояние)
    distance:    1.5,    // метров (виртуальное расстояние до экрана)

    // ── Инициализация ─────────────────────────────────────────────────────
    async init(canvasEl) {
      this.canvas = canvasEl;

      // WebGL2 с XR-совместимостью
      this.gl = canvasEl.getContext('webgl2', {
        xrCompatible:        true,
        antialias:           false,   // экономим на мощи
        alpha:               false,
        depth:               false,
        stencil:             false,
        preserveDrawingBuffer: false,
      });

      if (!this.gl) {
        throw new Error('WebGL2 не поддерживается');
      }

      this._buildShaders();
      this._buildGeometry();
      this._buildStatsOverlay();
      return this;
    },

    // ── Вертексный шейдер: виртуальный экран в 3D-пространстве ──────────
    _buildShaders() {
      const gl = this.gl;

      // Виртуальный экран: квад в мировых координатах.
      // Размер задаётся в JS через u_screenHalfW/H, расстояние — u_dist.
      // Для каждого глаза передаём настоящую view*projection матрицу XR.
      const vsSource = `#version 300 es
        in vec2 a_pos;
        in vec2 a_uv;
        uniform mat4  u_viewProj;    // projection * view матрица глаза от XR
        uniform float u_screenHalfW; // половина ширины в метрах
        uniform float u_screenHalfH; // половина высоты в метрах
        uniform float u_dist;        // расстояние в метрах
        uniform float u_yOffset;     // смещение по Y в мировых координатах
        out vec2 v_uv;
        void main() {
          vec4 worldPos = vec4(
            a_pos.x * u_screenHalfW,
            a_pos.y * u_screenHalfH + u_yOffset,
            -u_dist,
            1.0
          );
          gl_Position = u_viewProj * worldPos;
          v_uv = a_uv;
        }
      `;

      const fsSource = `#version 300 es
        precision mediump float;
        in vec2 v_uv;
        uniform sampler2D u_video;
        out vec4 outColor;
        void main() {
          outColor = texture(u_video, v_uv);
        }
      `;

      const vs = this._compileShader(gl.VERTEX_SHADER,   vsSource);
      const fs = this._compileShader(gl.FRAGMENT_SHADER, fsSource);

      this.program = gl.createProgram();
      gl.attachShader(this.program, vs);
      gl.attachShader(this.program, fs);
      gl.linkProgram(this.program);

      if (!gl.getProgramParameter(this.program, gl.LINK_STATUS)) {
        throw new Error('Shader link error: ' + gl.getProgramInfoLog(this.program));
      }

      // Локации uniform/attribute
      this._loc = {
        pos:          gl.getAttribLocation (this.program, 'a_pos'),
        uv:           gl.getAttribLocation (this.program, 'a_uv'),
        viewProj:     gl.getUniformLocation(this.program, 'u_viewProj'),
        screenHalfW:  gl.getUniformLocation(this.program, 'u_screenHalfW'),
        screenHalfH:  gl.getUniformLocation(this.program, 'u_screenHalfH'),
        dist:         gl.getUniformLocation(this.program, 'u_dist'),
        yOffset:      gl.getUniformLocation(this.program, 'u_yOffset'),
        video:        gl.getUniformLocation(this.program, 'u_video'),
      };
    },

    _compileShader(type, src) {
      const gl = this.gl;
      const sh = gl.createShader(type);
      gl.shaderSource(sh, src);
      gl.compileShader(sh);
      if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
        throw new Error('Shader compile: ' + gl.getShaderInfoLog(sh));
      }
      return sh;
    },

    // ── Геометрия: два треугольника = полноэкранный квад ─────────────────
    _buildGeometry() {
      const gl = this.gl;
      // [x, y,  u, v]  — NDC координаты и UV текстуры
      const verts = new Float32Array([
        -1, -1,  0, 1,
         1, -1,  1, 1,
        -1,  1,  0, 0,
         1,  1,  1, 0,
      ]);
      const idx = new Uint16Array([0, 1, 2,  2, 1, 3]);

      this.vao = gl.createVertexArray();
      gl.bindVertexArray(this.vao);

      const vbo = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
      gl.bufferData(gl.ARRAY_BUFFER, verts, gl.STATIC_DRAW);

      const ebo = gl.createBuffer();
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ebo);
      gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, idx, gl.STATIC_DRAW);

      const stride = 4 * 4; // 4 float × 4 байт
      gl.enableVertexAttribArray(this._loc.pos);
      gl.vertexAttribPointer(this._loc.pos, 2, gl.FLOAT, false, stride, 0);
      gl.enableVertexAttribArray(this._loc.uv);
      gl.vertexAttribPointer(this._loc.uv,  2, gl.FLOAT, false, stride, 8);

      gl.bindVertexArray(null);
    },

    // ── Stats overlay: 2D canvas → GL текстура ───────────────────────────
    _buildStatsOverlay() {
      const gl = this.gl;

      // Offscreen canvas для рендера текста
      this._statsCanvas = document.createElement('canvas');
      this._statsCanvas.width  = 768;
      this._statsCanvas.height = 88;
      this._statsCtx = this._statsCanvas.getContext('2d');

      // Кэш строк — обновляется из stats.js, читается внутри XR-фрейма
      this._statsLine1 = '';
      this._statsLine2 = '';
      this._statsDirty = false;

      // GL текстура
      this._statsTex = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, this._statsTex);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S,     gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T,     gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE,
        new Uint8Array([0, 0, 0, 0]));
    },

    // Перерисовывает 2D canvas и загружает в GL текстуру.
    // Вызывается ВНУТРИ XR-фрейма чтобы GL-контекст был валиден.
    _uploadStatsTexture() {
      const ctx = this._statsCtx;
      const w   = this._statsCanvas.width;
      const h   = this._statsCanvas.height;
      const r   = 12;

      ctx.clearRect(0, 0, w, h);

      if (!this._statsLine1) return;

      // Полупрозрачный фон
      ctx.fillStyle = 'rgba(0, 0, 0, 0.70)';
      ctx.beginPath();
      ctx.moveTo(r, 0); ctx.lineTo(w - r, 0);
      ctx.arcTo(w, 0, w, r, r);
      ctx.lineTo(w, h - r); ctx.arcTo(w, h, w - r, h, r);
      ctx.lineTo(r, h); ctx.arcTo(0, h, 0, h - r, r);
      ctx.lineTo(0, r); ctx.arcTo(0, 0, r, 0, r);
      ctx.closePath();
      ctx.fill();

      ctx.textAlign    = 'center';
      ctx.textBaseline = 'middle';

      // Строка 1 — задержки
      ctx.fillStyle = '#ffffff';
      ctx.font = 'bold 19px monospace';
      ctx.fillText(this._statsLine1, w / 2, this._statsLine2 ? h * 0.33 : h / 2);

      // Строка 2 — fps/res/codec
      if (this._statsLine2) {
        ctx.fillStyle = '#aaaaaa';
        ctx.font = '16px monospace';
        ctx.fillText(this._statsLine2, w / 2, h * 0.70);
      }

      const gl = this.gl;
      gl.bindTexture(gl.TEXTURE_2D, this._statsTex);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, this._statsCanvas);
    },

    /** Вызывается из stats.js каждую секунду — только кэшируем данные */
    updateStats(line1, line2) {
      this._statsLine1 = line1;
      this._statsLine2 = line2;
      this._statsDirty = true;
    },

    // ── Текстура из видео ─────────────────────────────────────────────────
    _createVideoTexture(videoEl) {
      const gl = this.gl;
      this.videoEl = videoEl;
      this.texture = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, this.texture);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S,     gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T,     gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    },

    // ── Запустить WebXR сессию ────────────────────────────────────────────
    async startXR(videoEl) {
      if (!navigator.xr) throw new Error('WebXR не поддерживается');

      const supported = await navigator.xr.isSessionSupported('immersive-vr');
      if (!supported) throw new Error('immersive-vr не поддерживается');

      this.session = await navigator.xr.requestSession('immersive-vr', {
        requiredFeatures: ['local'],
        optionalFeatures: ['local-floor', 'bounded-floor', 'hand-tracking'],
      });

      // Привязываем XR слой к нашему canvas
      const baseLayer = new XRWebGLLayer(this.session, this.gl, {
        antialias: false,
        depth:     false,
        stencil:   false,
        alpha:     false,
        framebufferScaleFactor: 1.0,   // 1.0 = нативное разрешение Quest
      });
      await this.session.updateRenderState({ baseLayer });

      // Попробовать local-floor (с данными Guardian), иначе local (без пола)
      this.xrRefSpace = await this.session.requestReferenceSpace('local-floor')
        .catch(() => this.session.requestReferenceSpace('local'));

      // Создать текстуру для видео
      this._createVideoTexture(videoEl);

      // Запустить рендер-цикл
      this.session.requestAnimationFrame(this._onXRFrame.bind(this));

      this.session.addEventListener('end', () => {
        console.log('[XR] session ended');
        this.session = null;
        FPVRenderer.onSessionEnd?.();
      });

      console.log('[XR] session started');
    },

    // ── Рендер каждого XR-кадра ───────────────────────────────────────────
    _onXRFrame(time, frame) {
      const session = frame.session;
      session.requestAnimationFrame(this._onXRFrame.bind(this));

      const gl    = this.gl;
      const layer = session.renderState.baseLayer;
      gl.bindFramebuffer(gl.FRAMEBUFFER, layer.framebuffer);

      // Обновляем текстуру из текущего кадра видео
      if (this.videoEl && this.videoEl.readyState >= this.videoEl.HAVE_CURRENT_DATA) {
        gl.bindTexture(gl.TEXTURE_2D, this.texture);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA,
          gl.RGBA, gl.UNSIGNED_BYTE, this.videoEl);
      }

      const pose = frame.getViewerPose(this.xrRefSpace);
      if (!pose) return;

      gl.useProgram(this.program);
      gl.bindVertexArray(this.vao);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, this.texture);
      gl.uniform1i(this._loc.video, 0);

      // ── Видео: 2.4м × 1.35м (16:9) на расстоянии 1.5м ──────────────────
      gl.uniform1f(this._loc.screenHalfW, 1.2);
      gl.uniform1f(this._loc.screenHalfH, 0.675);
      gl.uniform1f(this._loc.dist,        this.distance);
      gl.uniform1f(this._loc.yOffset,     0.0);

      for (const view of pose.views) {
        const vp = layer.getViewport(view);
        gl.viewport(vp.x, vp.y, vp.width, vp.height);
        const viewProj = this._mulMat4(view.projectionMatrix, view.transform.inverse.matrix);
        gl.uniformMatrix4fv(this._loc.viewProj, false, viewProj);
        gl.drawElements(gl.TRIANGLES, 6, gl.UNSIGNED_SHORT, 0);
      }

      // ── Stats HUD: маленький квад в world-space над видео ────────────────
      if (this._statsDirty) {
        this._uploadStatsTexture();
        this._statsDirty = false;
      }
      if (this._statsLine1) {
        gl.enable(gl.BLEND);
        gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);

        // Тот же VAO (this.vao), другие uniforms:
        // 0.5м wide × 0.07м tall, смещён на 0.72м вверх, на 0.8м вперёд
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, this._statsTex);
        gl.uniform1i(this._loc.video,       0);
        gl.uniform1f(this._loc.screenHalfW, 1.1);
        gl.uniform1f(this._loc.screenHalfH, 0.09);
        gl.uniform1f(this._loc.dist,        this.distance);
        gl.uniform1f(this._loc.yOffset,     0.77);

        for (const view of pose.views) {
          const vp = layer.getViewport(view);
          gl.viewport(vp.x, vp.y, vp.width, vp.height);
          const viewProj = this._mulMat4(view.projectionMatrix, view.transform.inverse.matrix);
          gl.uniformMatrix4fv(this._loc.viewProj, false, viewProj);
          gl.drawElements(gl.TRIANGLES, 6, gl.UNSIGNED_SHORT, 0);
        }

        gl.disable(gl.BLEND);
      }

      gl.bindVertexArray(null);

      // Экспортируем ориентацию головы для DataChannel
      if (pose.transform) {
        const q = pose.transform.orientation;
        FPVRenderer.onHeadPose?.({
          x: q.x, y: q.y, z: q.z, w: q.w,
          timestamp: time,
        });
      }
    },

    // ── Перемножение 4×4 матриц (column-major, как в WebGL) ──────────────
    _mulMat4(a, b) {
      const out = new Float32Array(16);
      for (let i = 0; i < 4; i++) {
        for (let j = 0; j < 4; j++) {
          let s = 0;
          for (let k = 0; k < 4; k++) s += a[i + k * 4] * b[k + j * 4];
          out[i + j * 4] = s;
        }
      }
      return out;
    },

    // ── Остановить XR ─────────────────────────────────────────────────────
    async stopXR() {
      if (this.session) {
        await this.session.end();
      }
    },

    // ── Колбэки ───────────────────────────────────────────────────────────
    onSessionEnd: null,
    onHeadPose:   null,   // (quaternion) => void
  };

  window.FPVRenderer = FPVRenderer;
})();
