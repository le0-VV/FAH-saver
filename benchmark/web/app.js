(() => {
  'use strict';
  const {limits, parseCapture, validateCapture, readCaptureURL} = globalThis.FAHCapture;
  const {sphereMesh, cylinderMesh, makeScene, viewProjection, styleAt, nextFrameDeadline} = globalThis.FAHScene;
  const canvas = document.getElementById('canvas');
  const status = document.getElementById('status');
  const resultElement = document.getElementById('result');
  const controls = document.getElementById('controls');
  const fields = Object.fromEntries(['capture', 'playback', 'style', 'fps', 'background', 'colour', 'seconds'].map(id => [id, document.getElementById(id)]));
  const params = new URLSearchParams(location.search);
  let gl, proteinProgram, backgroundProgram, atomMesh, bondMesh, uniform, backgroundUniform;
  let capture, scene, previousFrame = -1, previousStyle = -1, epoch = 0, nextDue = 0, animation = 0, metrics = null;
  let settings = {fps: 60, style: '1', playback: 'latest', background: 'sky', colour: [.02, .025, .035], seconds: 30, warmup: 0};

  const vertexSource = `#version 300 es
    precision highp float;
    layout(location=0) in vec3 position;
    layout(location=1) in vec3 normal;
    layout(location=2) in mat4 instance;
    layout(location=6) in vec4 colour;
    uniform mat4 viewProjection;
    uniform mat3 rotation;
    out vec3 vertexNormal;
    out vec4 vertexColour;
    void main() {
      vec4 clip = viewProjection * instance * vec4(position, 1.);
      clip.z = 2. * clip.z - clip.w;
      gl_Position = clip;
      vertexNormal = rotation * mat3(instance) * normal;
      vertexColour = colour;
    }`;
  const fragmentSource = `#version 300 es
    precision highp float;
    in vec3 vertexNormal;
    in vec4 vertexColour;
    out vec4 outputColour;
    void main() {
      float light = .25 + .75 * max(dot(normalize(vertexNormal), normalize(vec3(.4,.8,1.))), 0.);
      outputColour = vec4(vertexColour.rgb * light, 1.);
    }`;
  const backgroundVertexSource = `#version 300 es
    precision highp float;
    out vec2 uv;
    void main() {
      vec2 p = gl_VertexID == 0 ? vec2(-1.,-1.) : gl_VertexID == 1 ? vec2(3.,-1.) : vec2(-1.,3.);
      gl_Position = vec4(p,0.,1.);
      uv = vec2(p.x * .5 + .5, .5 - p.y * .5);
    }`;
  const backgroundFragmentSource = `#version 300 es
    precision highp float;
    in vec2 uv;
    uniform bool sky;
    uniform vec3 solid;
    out vec4 outputColour;
    void main() {
      vec3 colour = solid;
      if (sky) {
        colour = mix(vec3(.09,.23,.40), vec3(.55,.70,.80), uv.y);
        if (uv.y > .55) {
          float depth = (uv.y - .55) / .45;
          vec2 p = vec2((uv.x - .5) * 6., 3.) / max(depth, .02);
          vec2 grid = abs(fract(p - .5) - .5) / max(fwidth(p), vec2(.0001));
          float line = 1. - min(min(grid.x, grid.y), 1.);
          colour = mix(colour, vec3(.25,.38,.45), line * depth * .35);
        }
      }
      outputColour = vec4(colour,1.);
    }`;

  function reportError(error) {
    status.className = 'error';
    status.hidden = false;
    status.textContent = error instanceof Error ? error.message : String(error);
    if (metrics) finish('error', status.textContent);
  }

  function program(vertex, fragment) {
    const shaders = [];
    const output = gl.createProgram();
    try {
      for (const [kind, source] of [[gl.VERTEX_SHADER, vertex], [gl.FRAGMENT_SHADER, fragment]]) {
        const shader = gl.createShader(kind);
        shaders.push(shader);
        gl.shaderSource(shader, source); gl.compileShader(shader);
        if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(shader));
        gl.attachShader(output, shader);
      }
      gl.linkProgram(output);
      if (!gl.getProgramParameter(output, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(output));
      return output;
    } catch (error) { gl.deleteProgram(output); throw error; }
    finally { for (const shader of shaders) gl.deleteShader(shader); }
  }

  function uploadMesh(mesh) {
    const vao = gl.createVertexArray(), vertex = gl.createBuffer(), index = gl.createBuffer(), instances = gl.createBuffer();
    gl.bindVertexArray(vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, vertex); gl.bufferData(gl.ARRAY_BUFFER, mesh.vertices, gl.STATIC_DRAW);
    for (let i = 0; i < 2; i++) {
      gl.enableVertexAttribArray(i); gl.vertexAttribPointer(i, 3, gl.FLOAT, false, 24, i * 12);
    }
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, index); gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, mesh.indices, gl.STATIC_DRAW);
    gl.bindBuffer(gl.ARRAY_BUFFER, instances);
    for (let i = 0; i < 5; i++) {
      gl.enableVertexAttribArray(2 + i); gl.vertexAttribPointer(2 + i, 4, gl.FLOAT, false, 80, i * 16); gl.vertexAttribDivisor(2 + i, 1);
    }
    gl.bindVertexArray(null);
    return {vao, vertex, index, instances, indexCount: mesh.indices.length, count: 0};
  }

  function setInstances(mesh, data) {
    gl.bindBuffer(gl.ARRAY_BUFFER, mesh.instances);
    gl.bufferData(gl.ARRAY_BUFFER, data, gl.DYNAMIC_DRAW);
    mesh.count = data.length / 20;
  }

  function initialiseGL() {
    gl = canvas.getContext('webgl2', {alpha: false, antialias: true, depth: true, powerPreference: 'default'});
    if (!gl) throw new Error('WebGL 2 is unavailable');
    proteinProgram = program(vertexSource, fragmentSource);
    backgroundProgram = program(backgroundVertexSource, backgroundFragmentSource);
    uniform = {vp: gl.getUniformLocation(proteinProgram, 'viewProjection'), rotation: gl.getUniformLocation(proteinProgram, 'rotation')};
    backgroundUniform = {sky: gl.getUniformLocation(backgroundProgram, 'sky'), solid: gl.getUniformLocation(backgroundProgram, 'solid')};
    atomMesh = uploadMesh(sphereMesh()); bondMesh = uploadMesh(cylinderMesh());
    gl.disable(gl.BLEND); gl.disable(gl.CULL_FACE); gl.depthFunc(gl.LESS);
    canvas.addEventListener('webglcontextlost', event => {
      event.preventDefault(); cancelAnimationFrame(animation); animation = 0;
      reportError(new Error('WebGL context was lost. Reload before another trial.'));
    });
  }

  function readSettings() {
    const next = {...settings, fps: Number(fields.fps.value), style: fields.style.value,
      playback: fields.playback.value, background: fields.background.value, seconds: Number(fields.seconds.value)};
    if (!Number.isInteger(next.fps) || next.fps < 1 || next.fps > 240) throw new Error('Frame limit must be an integer from 1 to 240');
    if (!Number.isFinite(next.seconds) || next.seconds < 1 || next.seconds > 3600) throw new Error('Trial duration must be from 1 to 3600 seconds');
    styleAt(next.style, 0);
    if (!['latest', 'loop'].includes(next.playback)) throw new Error('Playback must be latest or loop');
    if (!['sky', 'solid'].includes(next.background)) throw new Error('Background must be sky or solid');
    settings = next;
  }

  function resize() {
    const width = Math.max(1, Math.round(canvas.clientWidth * devicePixelRatio));
    const height = Math.max(1, Math.round(canvas.clientHeight * devicePixelRatio));
    if (canvas.width !== width || canvas.height !== height) {
      if (metrics) throw new Error('Drawable dimensions changed during the trial');
      canvas.width = width; canvas.height = height;
    }
    gl.viewport(0, 0, canvas.width, canvas.height);
  }

  function draw(now) {
    resize();
    const seconds = (now - epoch) / 1000;
    const frame = settings.playback === 'latest' ? capture.frames.length - 1 : Math.floor(seconds * 10) % capture.frames.length;
    const style = styleAt(settings.style, seconds);
    if (frame !== previousFrame || style !== previousStyle) {
      scene = makeScene(capture, frame, style);
      setInstances(atomMesh, scene.atoms); setInstances(bondMesh, scene.bonds);
      previousFrame = frame; previousStyle = style;
    }
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    gl.disable(gl.DEPTH_TEST); gl.useProgram(backgroundProgram); gl.bindVertexArray(null);
    gl.uniform1i(backgroundUniform.sky, settings.background === 'sky'); gl.uniform3fv(backgroundUniform.solid, settings.colour);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    gl.enable(gl.DEPTH_TEST); gl.useProgram(proteinProgram);
    const angle = -seconds / 5, c = Math.cos(angle), s = Math.sin(angle);
    gl.uniformMatrix4fv(uniform.vp, false, viewProjection(angle, canvas.width / canvas.height, scene.radius));
    gl.uniformMatrix3fv(uniform.rotation, false, new Float32Array([c, 0, -s, 0, 1, 0, s, 0, c]));
    for (const mesh of [atomMesh, bondMesh]) if (mesh.count) {
      gl.bindVertexArray(mesh.vao); gl.drawElementsInstanced(gl.TRIANGLES, mesh.indexCount, gl.UNSIGNED_INT, 0, mesh.count);
    }
    if (metrics && now - metrics.lastStatus > 1000) {
      status.textContent = `Running: ${Math.max(0, (now - metrics.start) / 1000).toFixed(1)} / ${settings.seconds} s; ${capture.atoms.length} atoms`;
      metrics.lastStatus = now;
    }
  }

  function percentile(values, fraction) {
    if (!values.length) return null;
    const sorted = [...values].sort((a, b) => a - b);
    return sorted[Math.min(sorted.length - 1, Math.ceil(sorted.length * fraction) - 1)];
  }

  function finish(state = 'ok', error = null) {
    if (!metrics) return;
    const sample = metrics; metrics = null;
    const elapsed = Math.max(0, sample.lastDraw - sample.start);
    const debug = gl.getExtension('WEBGL_debug_renderer_info');
    const result = {
      status: state, error, renderer: 'WebGL2', timingKind: 'requestAnimationFrame callback and CPU submission; not presentation timestamps',
      width: canvas.width, height: canvas.height, devicePixelRatio, settings: {...settings},
      contextAttributes: gl.getContextAttributes(), adapter: gl.getParameter(debug ? debug.UNMASKED_RENDERER_WEBGL : gl.RENDERER),
      atoms: capture.atoms.length, bonds: capture.bonds.length, snapshots: capture.frames.length,
      elapsedSeconds: elapsed / 1000, drawCallbacks: sample.draws,
      drawCallbackFPS: elapsed > 0 ? Math.max(0, sample.draws - 1) * 1000 / elapsed : 0,
      drawCallbackIntervalsMs: {median: percentile(sample.drawIntervals, .5), p95: percentile(sample.drawIntervals, .95), p99: percentile(sample.drawIntervals, .99)},
      rafCallbackIntervalsMs: {median: percentile(sample.rafIntervals, .5), p95: percentile(sample.rafIntervals, .95), p99: percentile(sample.rafIntervals, .99)},
      cpuSubmitMs: {median: percentile(sample.cpu, .5), p95: percentile(sample.cpu, .95), p99: percentile(sample.cpu, .99)},
      gpuTimeMs: null, powerWatts: null,
      limitations: ['GPU time and power are unmeasured.', 'Callback cadence does not verify display presentation or the shipping frame-rate target.']
    };
    window.benchmarkResult = result;
    resultElement.hidden = params.get('benchmark') === '1'; resultElement.textContent = JSON.stringify(result, null, 2);
    status.textContent = state === 'ok' ? 'Trial complete. Callback cadence is not measured presentation.' : error;
    status.className = state === 'ok' ? '' : 'error';
    console.log('FAH_BENCHMARK_RESULT', JSON.stringify(result));
    window.webkit?.messageHandlers?.benchmark?.postMessage(result);
    window.dispatchEvent(new CustomEvent('fah-benchmark-result', {detail: result}));
  }

  function tick(now) {
    animation = requestAnimationFrame(tick);
    if (!capture || document.hidden) return;
    try {
      if (metrics && now >= metrics.start) {
        if (metrics.lastRaf) metrics.rafIntervals.push(now - metrics.lastRaf);
        metrics.lastRaf = now;
      }
      const deadline = nextFrameDeadline(now, nextDue, settings.fps);
      if (deadline === null) return;
      nextDue = deadline;
      const begin = performance.now(); draw(now); const end = performance.now();
      if (metrics && now >= metrics.start) {
        if (metrics.lastDraw) metrics.drawIntervals.push(now - metrics.lastDraw);
        else metrics.start = now;
        metrics.lastDraw = now; metrics.draws++; metrics.cpu.push(end - begin);
        if (now - metrics.start >= settings.seconds * 1000) finish();
      }
    } catch (error) { reportError(error); cancelAnimationFrame(animation); animation = 0; }
  }

  function start() {
    if (!capture) throw new Error('Load a capture before starting a trial');
    if (document.hidden) throw new Error('The benchmark must be visible');
    readSettings(); resize();
    window.benchmarkResult = null; resultElement.hidden = true;
    epoch = performance.now(); nextDue = epoch; previousFrame = -1; previousStyle = -1;
    metrics = {start: epoch + settings.warmup * 1000, lastDraw: 0, lastRaf: 0, lastStatus: epoch,
      draws: 0, drawIntervals: [], rafIntervals: [], cpu: []};
    status.className = ''; status.textContent = settings.warmup ? 'Warming up…' : 'Running…';
    if (!animation) animation = requestAnimationFrame(tick);
  }

  function setCapture(value) {
    if (metrics) finish('cancelled', 'Capture changed during the trial');
    capture = undefined;
    window.fahBenchmark.ready = false;
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    capture = typeof value === 'string' ? parseCapture(value) : validateCapture(value);
    previousFrame = -1; previousStyle = -1; epoch = performance.now(); nextDue = epoch;
    window.fahBenchmark.ready = true;
    status.className = ''; status.textContent = `${capture.atoms.length} atoms; ${capture.bonds.length} bonds; ${capture.frames.length} snapshots`;
    if (!animation) animation = requestAnimationFrame(tick);
    return true;
  }

  async function loadFile(file) {
    if (!file) return;
    if (file.size > limits.bytes) throw new Error('Capture exceeds 32 MiB');
    setCapture(new TextDecoder('utf-8', {fatal: true}).decode(await file.arrayBuffer()));
  }

  window.fahBenchmark = {ready: false, setCapture, start, stop: () => finish('cancelled', 'Stopped by caller')};
  controls.addEventListener('submit', event => { event.preventDefault(); try { start(); } catch (error) { reportError(error); } });
  fields.capture.addEventListener('change', () => loadFile(fields.capture.files[0]).catch(reportError));
  for (const name of ['fps', 'style', 'playback', 'background']) fields[name].addEventListener('change', () => {
    if (metrics) finish('cancelled', 'Settings changed during the trial');
    try { readSettings(); nextDue = performance.now(); } catch (error) { reportError(error); }
  });
  fields.colour.addEventListener('input', () => {
    if (metrics) finish('cancelled', 'Colour changed during the trial');
    settings.colour = [1, 3, 5].map(i => parseInt(fields.colour.value.slice(i, i + 2), 16) / 255);
  });
  document.addEventListener('dragover', event => event.preventDefault());
  document.addEventListener('drop', event => { event.preventDefault(); loadFile(event.dataTransfer.files[0]).catch(reportError); });
  document.addEventListener('visibilitychange', () => { if (document.hidden && metrics) finish('error', 'Page became hidden during the trial'); });
  window.addEventListener('pagehide', () => {
    cancelAnimationFrame(animation); animation = 0;
    if (!gl) return;
    for (const mesh of [atomMesh, bondMesh]) if (mesh) {
      for (const buffer of [mesh.vertex, mesh.index, mesh.instances]) gl.deleteBuffer(buffer);
      gl.deleteVertexArray(mesh.vao);
    }
    gl.deleteProgram(proteinProgram); gl.deleteProgram(backgroundProgram);
  });

  (async () => {
    try {
      for (const name of ['fps', 'style', 'playback', 'background', 'seconds']) if (params.has(name)) fields[name].value = params.get(name);
      if (params.has('warmup')) {
        settings.warmup = Number(params.get('warmup'));
        if (!Number.isFinite(settings.warmup) || settings.warmup < 0 || settings.warmup > 120) throw new Error('Warm-up must be from 0 to 120 seconds');
      }
      readSettings(); initialiseGL(); resize();
      if (params.get('benchmark') === '1') { controls.hidden = true; status.hidden = true; }
      if (window.nativeCapture !== undefined) setCapture(window.nativeCapture);
      else if (params.has('capture')) {
        capture = await readCaptureURL(params.get('capture'));
        previousFrame = -1; previousStyle = -1; epoch = performance.now(); nextDue = epoch;
        window.fahBenchmark.ready = true;
        status.textContent = `${capture.atoms.length} atoms; ${capture.frames.length} snapshots`;
        animation = requestAnimationFrame(tick);
      }
      if (capture && params.get('autostart') !== '0' && (params.has('seconds') || params.get('benchmark') === '1')) {
        if (document.hidden) await new Promise((resolve, reject) => {
          const onVisible = () => {
            if (!document.hidden) { clearTimeout(timeout); document.removeEventListener('visibilitychange', onVisible); resolve(); }
          };
          const timeout = setTimeout(() => {
            document.removeEventListener('visibilitychange', onVisible);
            reject(new Error('Page did not become visible within ten seconds'));
          }, 10000);
          document.addEventListener('visibilitychange', onVisible);
        });
        start();
      }
    } catch (error) {
      reportError(error);
      window.benchmarkResult = {status: 'error', error: status.textContent, renderer: 'WebGL2'};
      window.webkit?.messageHandlers?.benchmark?.postMessage(window.benchmarkResult);
    }
  })();
})();
