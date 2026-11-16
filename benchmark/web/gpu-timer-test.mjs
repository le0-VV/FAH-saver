import assert from 'node:assert/strict';
import './gpu-timer.js';

const {createGpuTimer} = globalThis.FAHGpuTimer;

function mockGL({supported = true, bits = 64, allocationFails = false} = {}) {
  const ext = {TIME_ELAPSED_EXT: 1, QUERY_COUNTER_BITS_EXT: 2, GPU_DISJOINT_EXT: 3};
  const state = {disjoint: false, lost: false, created: [], deleted: [], results: new Map(), active: null,
    availabilityReads: 0, resultReads: 0, endCalls: 0};
  const gl = {
    QUERY_RESULT_AVAILABLE: 4, QUERY_RESULT: 5,
    getExtension(name) { assert.equal(name, 'EXT_disjoint_timer_query_webgl2'); return supported ? ext : null; },
    getQuery(target, parameter) { assert.equal(target, 1); assert.equal(parameter, 2); return bits; },
    getParameter(parameter) {
      assert.equal(parameter, 3);
      const previous = state.disjoint; state.disjoint = false; return previous;
    },
    isContextLost() { return state.lost; },
    createQuery() {
      if (allocationFails) return null;
      const query = {id: state.created.length}; state.created.push(query); return query;
    },
    beginQuery(target, query) {
      assert.equal(target, 1); assert.equal(state.active, null);
      assert.ok(!state.deleted.includes(query)); state.results.delete(query); state.active = query;
    },
    endQuery(target) { assert.equal(target, 1); assert.ok(state.active); state.active = null; state.endCalls++; },
    getQueryParameter(query, parameter) {
      assert.ok(!state.deleted.includes(query));
      if (parameter === 4) { state.availabilityReads++; return state.results.has(query); }
      assert.equal(parameter, 5); assert.ok(state.results.has(query), 'Never read an unavailable result');
      state.resultReads++; return state.results.get(query);
    },
    deleteQuery(query) {
      assert.ok(!state.deleted.includes(query), 'Delete each query once'); state.deleted.push(query);
    }
  };
  return {gl, state};
}

for (const setup of [{supported: false}, {bits: 0}]) {
  const {gl, state} = mockGL(setup), timer = createGpuTimer(gl);
  assert.equal(timer.begin(), false); assert.equal(timer.begin(), false);
  const {info, samplesMs} = timer.finish();
  assert.equal(info.status, 'unsupported'); assert.equal(info.supported, false);
  assert.equal(info.attemptedFrames, 2); assert.equal(info.droppedFrames, 2);
  assert.deepEqual(samplesMs, []); assert.equal(state.created.length, 0);
}

{
  const {gl, state} = mockGL(), timer = createGpuTimer(gl);
  assert.ok(timer.begin()); timer.end();
  const query = state.created[0]; state.results.set(query, 2000000); timer.poll();
  assert.ok(timer.begin()); timer.end();
  state.results.set(query, 3000000); timer.poll();
  assert.ok(timer.begin()); timer.end();
  const {info, samplesMs} = timer.finish();
  assert.deepEqual(samplesMs, [2, 3]); assert.equal(info.status, 'measured');
  assert.equal(info.sampleCount, 2); assert.equal(info.submittedQueries, 3);
  assert.equal(info.completedQueries, 2); assert.equal(info.pendingAtFinish, 1);
  assert.equal(info.discardedQueries, 0); assert.equal(state.created.length, 1, 'Reuse completed query objects');
  assert.equal(state.deleted.length, 1); assert.equal(state.resultReads, 2);
}

{
  const {gl, state} = mockGL(), timer = createGpuTimer(gl);
  for (let i = 0; i < 12; i++) { if (timer.begin()) timer.end(); }
  const {info, samplesMs} = timer.finish();
  assert.equal(state.created.length, 8); assert.equal(state.deleted.length, 8);
  assert.equal(info.droppedPoolFull, 4); assert.equal(info.droppedFrames, 4);
  assert.equal(info.pendingAtFinish, 8); assert.equal(info.status, 'no-samples');
  assert.deepEqual(samplesMs, []); assert.equal(state.resultReads, 0);
  assert.ok(state.availabilityReads < 100, 'Poll a bounded pool once without waiting');
}

for (const failure of ['disjoint', 'context-lost']) {
  const {gl, state} = mockGL(), timer = createGpuTimer(gl);
  assert.ok(timer.begin()); timer.end();
  state.results.set(state.created[0], 1000000); timer.poll();
  assert.ok(timer.begin()); timer.end();
  if (failure === 'disjoint') state.disjoint = true;
  else state.lost = true;
  timer.poll();
  assert.equal(timer.begin(), false, 'Stop submitting queries after invalidation');
  const {info, samplesMs} = timer.finish();
  assert.equal(info.status, failure); assert.deepEqual(samplesMs, []);
  assert.equal(info.sampleCount, 0); assert.equal(info.invalidatedSamples, 1);
  assert.equal(info.discardedQueries, 1); assert.equal(info.droppedFrames, 1);
  assert.equal(state.deleted.length, state.created.length);
}

{
  const {gl, state} = mockGL(), timer = createGpuTimer(gl);
  assert.ok(timer.begin());
  state.lost = true; timer.contextLost();
  const {info} = timer.finish();
  assert.equal(info.status, 'context-lost'); assert.equal(info.discardedQueries, 1);
  assert.equal(state.endCalls, 0, 'Do not end a query on a lost context');
  assert.equal(state.deleted.length, 1);
}

for (const invalid of [-1, Infinity, 1.5, Number.MAX_SAFE_INTEGER + 1]) {
  const {gl, state} = mockGL(), timer = createGpuTimer(gl);
  assert.ok(timer.begin()); timer.end();
  state.results.set(state.created[0], invalid);
  const {info, samplesMs} = timer.finish();
  assert.equal(info.status, 'error'); assert.deepEqual(samplesMs, []);
  assert.equal(state.deleted.length, 1);
}

{
  const {gl, state} = mockGL({allocationFails: true}), timer = createGpuTimer(gl);
  assert.equal(timer.begin(), false);
  const {info} = timer.finish();
  assert.equal(info.status, 'error'); assert.equal(info.droppedFrames, 1);
  assert.equal(state.deleted.length, 0);
}

{
  const {gl, state} = mockGL(), timer = createGpuTimer(gl);
  assert.ok(timer.begin());
  const result = timer.finish(), calls = state.availabilityReads;
  assert.equal(result.info.pendingAtFinish, 1); assert.equal(state.endCalls, 1);
  assert.equal(state.deleted.length, 1);
  assert.equal(timer.finish(), result, 'Cleanup is idempotent');
  assert.equal(state.availabilityReads, calls);
}

console.log('Browser GPU query support, reuse, bounds, invalidation and cleanup checks passed.');
