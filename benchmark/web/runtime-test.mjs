import assert from 'node:assert/strict';
import fs from 'node:fs';
import './capture.js';
import './scene.js';
import './gpu-timer.js';
import './renderer.js';
import './saver.js';

const fixture = {topology: {atoms: [['C', 0, 0, 0, 6], ['O', 0, 0, 0, 8]], bonds: [[0, 1]]},
  frames: [[[0, 0, 0], [1, 0, 0]], [[0, 0, 0], [0, 1, 0]]]};

function harness() {
  let clock = 1000, nextID = 1, resources = 1;
  const queued = new Map(), state = {deletedBuffers: [], deletedPrograms: [], deletedVAOs: [],
    loseCalls: 0, contexts: [], skyValues: [], uploads: [], shaderSources: [], draws: 0, extensions: []};
  const gl = Object.fromEntries(['VERTEX_SHADER', 'FRAGMENT_SHADER', 'COMPILE_STATUS', 'LINK_STATUS',
    'ARRAY_BUFFER', 'ELEMENT_ARRAY_BUFFER', 'STATIC_DRAW', 'DYNAMIC_DRAW', 'FLOAT', 'BLEND', 'LESS',
    'CCW', 'BACK', 'CULL_FACE', 'COLOR_BUFFER_BIT', 'DEPTH_BUFFER_BIT', 'DEPTH_TEST', 'TRIANGLES',
    'UNSIGNED_INT', 'RENDERER'].map((name, i) => [name, i + 1]));
  Object.assign(gl, {
    createProgram: () => ({id: resources++}), createShader: () => ({id: resources++}),
    createVertexArray: () => ({id: resources++}), createBuffer: () => ({id: resources++}),
    shaderSource: (_, source) => state.shaderSources.push(source),
    shaderSourceNoop() {}, compileShader() {}, attachShader() {}, linkProgram() {}, deleteShader() {},
    getShaderParameter: () => true, getProgramParameter: () => true,
    getUniformLocation: (_, name) => name,
    bindVertexArray() {}, bindBuffer() {}, enableVertexAttribArray() {}, vertexAttribPointer() {}, vertexAttribDivisor() {},
    disable() {}, enable() {}, depthFunc() {}, frontFace() {}, cullFace() {}, viewport() {}, clear() {}, useProgram() {},
    uniform1i: (_, value) => state.skyValues.push(value), uniform3fv() {}, uniformMatrix4fv() {}, uniformMatrix3fv() {},
    bufferData: (_, data) => state.uploads.push(data.slice()), drawArrays: () => state.draws++, drawElementsInstanced() {},
    deleteBuffer: value => state.deletedBuffers.push(value), deleteProgram: value => state.deletedPrograms.push(value),
    deleteVertexArray: value => state.deletedVAOs.push(value),
    getExtension(name) {
      state.extensions.push(name);
      return name === 'WEBGL_lose_context' ? {loseContext: () => state.loseCalls++} : null;
    },
    getParameter: () => 'Test GPU', getContextAttributes: () => ({alpha: false, antialias: false, depth: true})
  });
  const listeners = new Map();
  const canvas = {clientWidth: 160, clientHeight: 100, width: 0, height: 0,
    getContext(kind, options) { assert.equal(kind, 'webgl2'); state.contexts.push(options); return gl; },
    addEventListener: (name, callback) => listeners.set(name, callback),
    removeEventListener: name => listeners.delete(name)};
  globalThis.devicePixelRatio = 2;
  const runtime = globalThis.FAHSaverRuntime.createRuntime({canvas, now: () => clock,
    requestFrame: callback => { const id = nextID++; queued.set(id, callback); return id; },
    cancelFrame: id => queued.delete(id), onError: error => { state.error = error; },
    onFirstFrame: info => { state.firstFrames = (state.firstFrames || 0) + 1; state.firstFrame = info; }});
  return {runtime, canvas, state, queued, listeners, advance(milliseconds) {
    clock += milliseconds;
    const callbacks = [...queued.values()]; queued.clear();
    for (const callback of callbacks) callback(clock);
  }};
}

{
  const h = harness(), r = h.runtime;
  r.start();
  assert.equal(h.queued.size, 0, 'An empty model must not spin a background draw loop');
  assert.equal(h.state.firstFrames || 0, 0);
  r.setCapture(fixture);
  assert.equal(r.metrics().snapshotCount, 1, 'Latest mode keeps only the newest frame');
  assert.equal(h.queued.size, 1);
  h.advance(0);
  assert.equal(r.metrics().width, 320); assert.equal(r.metrics().height, 200);
  assert.equal(h.state.firstFrames, 1);
  h.advance(2000);
  const before = r.metrics().elapsedSeconds;
  assert.equal(before, 2);
  r.replaceLatest(fixture.frames[0]);
  assert.equal(r.metrics().elapsedSeconds, before);
  h.advance(20); assert.equal(r.metrics().elapsedSeconds, 2.02, 'Replacing a live frame preserves rotation time');
  r.setCapture(fixture); h.advance(20);
  assert.equal(r.metrics().elapsedSeconds, 2.04, 'Topology handover preserves rotation time');
  const stale = [...h.queued.values()][0], draws = r.metrics().draws;
  r.stop(); assert.equal(h.queued.size, 0);
  stale(4000); assert.equal(r.metrics().draws, draws, 'A cancelled generation cannot draw or restart scheduling');
  r.start(); r.start(); assert.equal(h.queued.size, 1, 'Repeated start is idempotent');
  h.advance(0); assert.equal(r.metrics().elapsedSeconds, 0);
  assert.equal(h.state.firstFrames, 2);
  r.setCapture(null);
  assert.equal(r.metrics().snapshotCount, 0); assert.equal(h.queued.size, 0);
  assert.throws(() => r.replaceLatest(fixture.frames[0]), /topology/);
  assert.throws(() => r.appendFrames([fixture.frames[0]]), /topology/);
  r.dispose(); r.dispose();
  assert.equal(h.state.deletedBuffers.length, 6); assert.equal(new Set(h.state.deletedBuffers).size, 6);
  assert.equal(h.state.deletedPrograms.length, 2); assert.equal(h.state.deletedVAOs.length, 2);
  assert.equal(h.state.loseCalls, 1); assert.equal(h.listeners.size, 0);
  assert.equal(h.queued.size, 0);
  assert.throws(() => r.start(), /disposed/);
  assert.throws(() => r.renderAt(0), /disposed/);
}

{
  const h = harness(), r = h.runtime;
  r.setCapture(fixture);
  assert.equal(r.metrics().draws, 0);
  r.start();
  assert.equal(r.metrics().draws, 1, 'A cached model must draw synchronously before the first rAF');
  assert.equal(h.state.firstFrames, 1, 'The first model frame is ready before animation callbacks');
  assert.equal(r.metrics().elapsedSeconds, 0);
  assert.equal(h.queued.size, 1, 'Startup schedules exactly one animation callback');
  r.start();
  assert.equal(r.metrics().draws, 1, 'An idempotent start must not redraw or schedule twice');
  assert.equal(h.queued.size, 1);
  h.advance(0);
  assert.equal(r.metrics().draws, 1, 'A same-refresh callback must not duplicate the synchronous startup frame');
  h.advance(20);
  assert.equal(r.metrics().elapsedSeconds, .02, 'Animation continues from the startup epoch');
  assert.equal(h.state.firstFrames, 1, 'The scheduled frame must not repeat first-frame readiness');
  r.dispose();
}

{
  const h = harness(), r = h.runtime;
  r.configure({playback: 'loop'}); r.setCapture(fixture); r.renderAt(.99);
  assert.equal(r.metrics().snapshotIndex, 0);
  r.renderAt(1); assert.equal(r.metrics().snapshotIndex, 1);
  r.appendFrames([fixture.frames[0]]); assert.equal(r.metrics().snapshotCount, 3);
  r.renderAt(2); assert.equal(r.metrics().snapshotIndex, 2, 'Loop append preserves frame order');
  r.configure({snapshotInterval: 1e100}); r.renderAt(10000);
  assert.equal(r.metrics().snapshotIndex, 0, 'Snapshot interval has no artificial upper cap');
  r.configure({background: 'dark'}); r.renderAt(0); assert.equal(h.state.skyValues.at(-1), 2);
  r.configure({background: 'sky'}); r.renderAt(0); assert.equal(h.state.skyValues.at(-1), 1);
  r.configure({background: 'solid', colour: [.2, .3, .4]}); r.renderAt(0); assert.equal(h.state.skyValues.at(-1), 0);
  assert.ok(h.state.shaderSources.some(source => source.includes('vec3(11.,18.,29.) / 255.')));
  r.configure({style: 2, styleMode: 'cycle'}); r.renderAt(10); assert.equal(r.metrics().style, 3);
  r.configure({playback: 'latest'}); assert.equal(r.metrics().snapshotCount, 1);
  assert.throws(() => r.configure({snapshotInterval: .01}), /interval/);
  assert.throws(() => r.configure({snapshotInterval: Infinity}), /interval/);
  assert.throws(() => r.configure({snapshotInterval: '1'}), /interval/);
  assert.throws(() => r.configure({fps: 241}), /Frame limit/);
  assert.throws(() => r.configure({style: 0}), /Style/);
  assert.throws(() => r.configure({styleMode: 'bad'}), /Style mode/);
  assert.throws(() => r.configure({background: 'bad'}), /Background/);
  assert.throws(() => r.configure({colour: [NaN, 0, 0]}), /Colour/);
  assert.throws(() => r.configure({toString: 1}), /Unknown/);
  assert.throws(() => r.configure({collectMetrics: 'false'}), /boolean/);
  r.dispose();
}

{
  const h = harness(), r = h.runtime;
  r.configure({playback: 'loop'});
  r.setCapture({...fixture, frames: Array(10001).fill(fixture.frames[0])});
  assert.equal(r.metrics().snapshotCount, 10001, 'Live retention is not the developer capture frame cap');
  assert.throws(() => r.appendFrames(Array(100001)), /limit/);
  const before = r.metrics().snapshotCount;
  assert.throws(() => r.appendFrames([fixture.frames[0], [[NaN, 0, 0], [1, 1, 1]]]), /coordinates/);
  assert.equal(r.metrics().snapshotCount, before, 'A failed batch is atomic');
  const {liveLimits} = globalThis.FAHSaverRuntime;
  assert.equal(liveLimits.frames, 100000);
  assert.ok(liveLimits.positions * 12 <= 128 * 1024 * 1024);
  assert.throws(() => globalThis.FAHCapture.validateFrames([fixture.frames[0]], 2, 1), /budget/);
  r.dispose();
}

{
  const h = harness(), r = h.runtime;
  r.setCapture(fixture); r.start(); h.advance(0); h.advance(20);
  assert.equal(r.metrics().gpuTiming.status, 'disabled');
  assert.ok(!h.state.extensions.includes('EXT_disjoint_timer_query_webgl2'));
  r.beginMeasurement();
  assert.equal(r.metrics().measurementDraws, 0); assert.equal(r.metrics().settings.collectMetrics, true);
  assert.equal(r.metrics().elapsedSeconds, .02, 'Measurement reset preserves camera time');
  h.advance(20); h.advance(20);
  const m = r.metrics();
  assert.equal(m.measurementDraws, 2); assert.equal(m.cpuSubmitMs.samples, 2);
  assert.equal(m.gpuTiming.status, 'unsupported'); assert.equal(m.gpuTimeMs, null);
  assert.equal(m.drawCallbackFPS, 50);
  r.stop(); const stopped = r.metrics(); h.advance(200);
  assert.equal(r.metrics().draws, stopped.draws);
  assert.equal(r.metrics().measurementDraws, stopped.measurementDraws);
  r.configure({collectMetrics: false}); assert.equal(r.metrics().gpuTiming.status, 'disabled');
  r.dispose();
}

{
  const h = harness(), r = h.runtime;
  r.setCapture(fixture); r.start(); h.advance(0);
  h.listeners.get('webglcontextlost')({preventDefault() {}});
  assert.match(h.state.error, /context was lost/);
  assert.equal(r.metrics().running, false); assert.equal(h.queued.size, 0);
  assert.throws(() => r.start(), /context was lost/);
  r.dispose();
}

const page = fs.readFileSync(new URL('./saver.html', import.meta.url), 'utf8');
assert.match(page, /default-src 'none'/); assert.match(page, /connect-src 'none'/); assert.match(page, /style-src 'self'/);
assert.doesNotMatch(page, /<form|type="file"|https?:\/\//);
console.log('Saver stream limits, clocks, styles, backgrounds, lifecycle, disposal and optional measurement checks passed.');
