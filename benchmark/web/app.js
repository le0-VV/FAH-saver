(() => {
  'use strict';
  const {limits, parseCapture, validateCapture, readCaptureURL} = globalThis.FAHCapture;
  const {styleAt, elapsedSeconds, nextFrameDeadline} = globalThis.FAHScene;
  const {createRenderer} = globalThis.FAHWebRenderer;
  const {createGpuTimer} = globalThis.FAHGpuTimer;
  const canvas = document.getElementById('canvas');
  const status = document.getElementById('status');
  const resultElement = document.getElementById('result');
  const controls = document.getElementById('controls');
  const fields = Object.fromEntries(['capture', 'playback', 'style', 'fps', 'background', 'colour', 'seconds'].map(id => [id, document.getElementById(id)]));
  const params = new URLSearchParams(location.search);
  let renderer, gl;
  let capture, epoch = 0, nextDue = 0, animation = 0, metrics = null;
  let settings = {fps: 60, style: '1', playback: 'latest', background: 'sky', colour: [.02, .025, .035], seconds: 30, warmup: 0, snapshotInterval: .1};

  function reportError(error) {
    status.className = 'error';
    status.hidden = false;
    status.textContent = error instanceof Error ? error.message : String(error);
    if (metrics) finish('error', status.textContent);
  }

  function initialiseGL() {
    renderer = createRenderer(canvas, {onContextLost: error => {
      cancelAnimationFrame(animation); animation = 0; metrics?.gpuTimer.contextLost(); reportError(error);
    }});
    gl = renderer.gl; configureRenderer();
  }
  function configureRenderer() {
    renderer?.configure({fps: settings.fps, style: /^[123]$/.test(settings.style) ? Number(settings.style) : 1,
      styleMode: /^[123]$/.test(settings.style) ? 'fixed' : settings.style,
      playback: settings.playback, background: settings.background, colour: settings.colour,
      snapshotInterval: settings.snapshotInterval, styleInterval: 10});
  }

  function readSettings() {
    const next = {...settings, fps: Number(fields.fps.value), style: fields.style.value,
      playback: fields.playback.value, background: fields.background.value, seconds: Number(fields.seconds.value)};
    if (!Number.isInteger(next.fps) || next.fps < 1 || next.fps > 240) throw new Error('Frame limit must be an integer from 1 to 240');
    if (!Number.isFinite(next.seconds) || next.seconds < 1 || next.seconds > 3600) throw new Error('Trial duration must be from 1 to 3600 seconds');
    styleAt(next.style, 0);
    if (!['latest', 'loop'].includes(next.playback)) throw new Error('Playback must be latest or loop');
    if (!['sky', 'dark', 'solid'].includes(next.background)) throw new Error('Background must be sky, dark or solid');
    settings = next; configureRenderer();
  }

  function resize() { renderer.resize(!!metrics); }
  function draw(now, gpuTimer) {
    renderer.draw(elapsedSeconds(now, epoch), gpuTimer, !!metrics);
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
    if (params.get('benchmark') === '1') { cancelAnimationFrame(animation); animation = 0; }
    const elapsed = Math.max(0, sample.lastDraw - sample.start);
    const gpu = sample.gpuTimer.finish();
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
      gpuTimeMs: gpu.samplesMs.length ? {mean: gpu.samplesMs.reduce((sum, value) => sum + value, 0) / gpu.samplesMs.length,
        median: percentile(gpu.samplesMs, .5), p95: percentile(gpu.samplesMs, .95), p99: percentile(gpu.samplesMs, .99)} : null,
      gpuTiming: gpu.info, powerWatts: null,
      limitations: ['Power is unmeasured.',
        ...(gpu.info.status === 'measured' ? [] : [`GPU time is unmeasured: ${gpu.info.reason}`]),
        'GPU queries exclude uploads and presentation; final unavailable query results are excluded.',
        'Callback cadence does not verify display presentation or the shipping frame-rate target.']
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
        metrics.gpuTimer.poll();
        if (metrics.lastRaf) metrics.rafIntervals.push(now - metrics.lastRaf);
        metrics.lastRaf = now;
      }
      const deadline = nextFrameDeadline(now, nextDue, settings.fps);
      if (deadline === null) return;
      nextDue = deadline;
      const begin = performance.now(); draw(now, metrics && now >= metrics.start ? metrics.gpuTimer : null); const end = performance.now();
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
    if (metrics) finish('cancelled', 'A new trial started');
    readSettings(); resize();
    window.benchmarkResult = null; resultElement.hidden = true;
    epoch = performance.now(); nextDue = epoch;
    metrics = {start: epoch + settings.warmup * 1000, lastDraw: 0, lastRaf: 0, lastStatus: epoch,
      draws: 0, drawIntervals: [], rafIntervals: [], cpu: [], gpuTimer: createGpuTimer(gl)};
    status.className = ''; status.textContent = settings.warmup ? 'Warming up…' : 'Running…';
    if (!animation) animation = requestAnimationFrame(tick);
  }

  function setCapture(value) {
    if (metrics) finish('cancelled', 'Capture changed during the trial');
    capture = undefined;
    window.fahBenchmark.ready = false;
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    capture = typeof value === 'string' ? parseCapture(value) : validateCapture(value);
    renderer.setCapture(capture); epoch = performance.now(); nextDue = epoch;
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
    settings.colour = [1, 3, 5].map(i => parseInt(fields.colour.value.slice(i, i + 2), 16) / 255); configureRenderer();
  });
  document.addEventListener('dragover', event => event.preventDefault());
  document.addEventListener('drop', event => { event.preventDefault(); loadFile(event.dataTransfer.files[0]).catch(reportError); });
  document.addEventListener('visibilitychange', () => { if (document.hidden && metrics) finish('error', 'Page became hidden during the trial'); });
  window.addEventListener('pagehide', () => {
    cancelAnimationFrame(animation); animation = 0;
    if (metrics) finish('cancelled', 'Page closed during the trial');
    renderer?.dispose();
  });

  (async () => {
    try {
      for (const name of ['fps', 'style', 'playback', 'background', 'seconds']) if (params.has(name)) fields[name].value = params.get(name);
      if (params.has('snapshotInterval')) {
        settings.snapshotInterval = Number(params.get('snapshotInterval'));
        if (!Number.isFinite(settings.snapshotInterval) || settings.snapshotInterval < .05) throw new Error('Snapshot interval must be at least 0.05 seconds');
      }
      if (params.has('warmup')) {
        settings.warmup = Number(params.get('warmup'));
        if (!Number.isFinite(settings.warmup) || settings.warmup < 0 || settings.warmup > 120) throw new Error('Warm-up must be from 0 to 120 seconds');
      }
      readSettings(); initialiseGL(); resize();
      if (params.get('benchmark') === '1') { controls.hidden = true; status.hidden = true; }
      if (window.nativeCapture !== undefined) {
        setCapture(window.nativeCapture);
        delete window.nativeCapture;
      }
      else if (params.has('capture')) {
        capture = await readCaptureURL(params.get('capture'));
        renderer.setCapture(capture); epoch = performance.now(); nextDue = epoch;
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
