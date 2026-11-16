(() => {
  'use strict';
  const {parseCapture, validateCapture, validateFrames, limits} = globalThis.FAHCapture;
  const {normaliseOptions, createRenderer} = globalThis.FAHWebRenderer;
  const {elapsedSeconds, nextFrameDeadline} = globalThis.FAHScene;
  const liveLimits = Object.freeze({...limits, frames: 100000, positions: Math.floor(128 * 1024 * 1024 / 12)});

  function summary(values) {
    if (!values.length) return {samples: 0};
    const sorted = values.slice().sort((a, b) => a - b);
    const percentile = p => sorted[Math.min(sorted.length - 1, Math.ceil(sorted.length * p) - 1)];
    return {samples: sorted.length, mean: values.reduce((a, b) => a + b, 0) / values.length,
      p50: percentile(.5), p95: percentile(.95), p99: percentile(.99), max: sorted[sorted.length - 1]};
  }

  function createRuntime({canvas, renderer, now = () => performance.now(),
    requestFrame = callback => requestAnimationFrame(callback), cancelFrame = id => cancelAnimationFrame(id),
    onError = () => {}, onFirstFrame = () => {}}) {
    let settings = normaliseOptions(), capture = null, running = false, disposed = false, generation = 0;
    let animation = 0, epoch = 0, nextDue = 0, lastSeconds = 0, lastError = null, notifiedFrame = false;
    let measurement = null, finalGpu = null;
    const engine = renderer || createRenderer(canvas, {onContextLost: fail});

    function requireActive() {
      if (disposed) throw new Error('Saver runtime has been disposed');
      if (lastError) throw new Error(lastError);
    }
    function addSample(list, value) {
      if (list.length < 100000) list.push(value);
      else measurement.droppedSamples++;
    }
    function fail(error) {
      lastError = error instanceof Error ? error.message : String(error);
      stop(); onError(lastError);
    }
    function schedule() {
      if (!running || !capture || animation) return;
      const token = generation;
      animation = requestFrame(timestamp => tick(timestamp, token));
    }
    function draw(seconds, timestamp = now(), record = false) {
      const before = now();
      const result = engine.draw(seconds, record ? measurement?.gpuTimer : null);
      lastSeconds = seconds;
      if (record && measurement && result) {
        if (measurement.lastDraw !== null) addSample(measurement.drawIntervals, timestamp - measurement.lastDraw);
        else measurement.firstDraw = timestamp;
        measurement.lastDraw = timestamp; measurement.draws++;
        addSample(measurement.cpu, now() - before);
      }
      if (result && capture && !notifiedFrame) {
        notifiedFrame = true;
        const {draws, width, height} = engine.info(); onFirstFrame({draws, width, height});
      }
      return result;
    }
    function tick(timestamp, token) {
      if (!running || disposed || token !== generation) return;
      animation = 0;
      if (!capture) return;
      try {
        if (measurement) {
          measurement.gpuTimer?.poll();
          if (measurement.lastRaf !== null) addSample(measurement.rafIntervals, timestamp - measurement.lastRaf);
          measurement.lastRaf = timestamp;
        }
        const deadline = nextFrameDeadline(timestamp, nextDue, settings.fps);
        if (deadline !== null) {
          nextDue = deadline;
          draw(elapsedSeconds(timestamp, epoch), timestamp, !!measurement);
        }
        schedule();
      } catch (error) { fail(error); }
    }
    function beginMeasurement() {
      requireActive();
      measurement?.gpuTimer?.finish();
      settings = {...settings, collectMetrics: true}; engine.configure(settings);
      const gpuTimer = globalThis.FAHGpuTimer.createGpuTimer(engine.gl);
      measurement = {started: now(), firstDraw: null, lastDraw: null, lastRaf: null, draws: 0,
        cpu: [], drawIntervals: [], rafIntervals: [], droppedSamples: 0, gpuTimer};
      finalGpu = null;
      return true;
    }
    function configure(value) {
      requireActive();
      const next = normaliseOptions(value, settings);
      const metricsChanged = next.collectMetrics !== settings.collectMetrics;
      settings = next; engine.configure(settings);
      if (capture && settings.playback === 'latest' && capture.frames.length > 1)
        capture.frames = [capture.frames[capture.frames.length - 1]];
      nextDue = now();
      if (metricsChanged && settings.collectMetrics) beginMeasurement();
      else if (metricsChanged) {
        measurement?.gpuTimer?.finish(); measurement = null; finalGpu = null;
      }
      if (running && !capture) draw(elapsedSeconds(now(), epoch));
      return {...settings, colour: settings.colour.slice()};
    }
    function setCapture(value) {
      requireActive();
      if (value === null) {
        capture = null; engine.setCapture(null);
        if (animation) cancelFrame(animation);
        animation = 0;
        draw(running ? elapsedSeconds(now(), epoch) : lastSeconds);
        return true;
      }
      const next = typeof value === 'string' ? parseCapture(value, liveLimits) : validateCapture(value, liveLimits);
      if (settings.playback === 'latest') next.frames = [next.frames[next.frames.length - 1]];
      const firstModel = !capture;
      capture = next; engine.setCapture(capture);
      if (running && firstModel) {
        const timestamp = now();
        draw(elapsedSeconds(timestamp, epoch), timestamp);
        nextDue = timestamp + 1000 / settings.fps;
      }
      schedule(); return true;
    }
    function updateFrames(value, replace) {
      requireActive();
      if (!capture) throw new Error('A topology and initial frame are required before frame updates');
      const raw = replace ? [value] : value;
      if (!Array.isArray(raw) || raw.length > liveLimits.frames)
        throw new Error('Frame update exceeds the 100,000 snapshot limit');
      const retained = settings.playback === 'loop' && !replace ? capture.frames.length : 0;
      if (retained + raw.length > liveLimits.frames || (retained + raw.length) * capture.atoms.length > liveLimits.positions)
        throw new Error('Visualisation history exceeds the 128 MiB or 100,000 snapshot limit');
      const frames = validateFrames(raw, capture.atoms.length, liveLimits.positions);
      if (!frames.length) return true;
      if (settings.playback === 'latest' || replace) capture.frames = [frames[frames.length - 1]];
      else {
        const count = capture.frames.length + frames.length;
        if (count > liveLimits.frames || count * capture.atoms.length > liveLimits.positions)
          throw new Error('Visualisation history exceeds the 128 MiB or 100,000 snapshot limit');
        for (const frame of frames) capture.frames.push(frame);
      }
      schedule(); return true;
    }
    function start() {
      requireActive();
      if (running) return true;
      running = true; generation++; epoch = now(); nextDue = epoch; lastSeconds = 0; notifiedFrame = false;
      if (settings.collectMetrics) beginMeasurement();
      draw(0);
      if (capture) nextDue = epoch + 1000 / settings.fps;
      schedule(); return true;
    }
    function stop() {
      if (animation) cancelFrame(animation);
      animation = 0; running = false; generation++;
      if (measurement?.gpuTimer) { finalGpu = measurement.gpuTimer.finish(); measurement.gpuTimer = null; }
      return true;
    }
    function dispose() {
      if (disposed) return true;
      stop(); disposed = true; capture = null; engine.dispose(); return true;
    }
    function metrics() {
      const info = engine.info(), gpu = measurement?.gpuTimer?.snapshot() || finalGpu;
      const measured = measurement?.draws || 0;
      const elapsed = measured > 1 ? (measurement.lastDraw - measurement.firstDraw) / 1000 : 0;
      return {...info, running, disposed, lastError, elapsedSeconds: lastSeconds,
        settings: {...settings, colour: settings.colour.slice()},
        measurementDraws: measured, drawCallbackFPS: elapsed > 0 ? (measured - 1) / elapsed : 0,
        measurementElapsedSeconds: measurement ? Math.max(0, now() - measurement.started) / 1000 : 0,
        cpuSubmitMs: summary(measurement?.cpu || []), drawCallbackIntervalsMs: summary(measurement?.drawIntervals || []),
        rafCallbackIntervalsMs: summary(measurement?.rafIntervals || []), droppedMetricSamples: measurement?.droppedSamples || 0,
        gpuTimeMs: gpu?.samplesMs.length ? summary(gpu.samplesMs) : null,
        gpuTiming: gpu?.info || {status: 'disabled', supported: null, reason: 'GPU timing was not enabled.'},
        timingKind: 'requestAnimationFrame callback and CPU submission; not presentation timestamps'};
    }
    return {configure, setCapture, appendFrames: frames => updateFrames(frames, false),
      replaceLatest: frame => updateFrames(frame, true), start, stop, dispose, beginMeasurement, metrics,
      renderAt(seconds) { requireActive(); return draw(seconds); }};
  }

  globalThis.FAHSaverRuntime = {createRuntime, liveLimits};
  if (typeof document !== 'undefined' && document.getElementById('canvas')) {
    const post = value => globalThis.webkit?.messageHandlers?.renderer?.postMessage(value);
    try {
      globalThis.fahSaver = createRuntime({canvas: document.getElementById('canvas'),
        onError: message => post({type: 'error', message}),
        onFirstFrame: info => post({type: 'frame', ...info})});
      addEventListener('pagehide', () => globalThis.fahSaver.dispose(), {once: true});
      post({type: 'ready'});
    } catch (error) { post({type: 'error', message: error.message || String(error)}); }
  }
})();
