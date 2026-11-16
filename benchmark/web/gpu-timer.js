(() => {
  'use strict';

  function createGpuTimer(gl) {
    const extensionName = 'EXT_disjoint_timer_query_webgl2', maxPendingQueries = 8;
    const ext = gl.getExtension(extensionName);
    const supported = !!ext && gl.getQuery(ext.TIME_ELAPSED_EXT, ext.QUERY_COUNTER_BITS_EXT) > 0;
    const owned = new Set(), idle = [], pending = [], samplesMs = [];
    const counts = {attemptedFrames: 0, submittedQueries: 0, completedQueries: 0,
      droppedFrames: 0, droppedPoolFull: 0, droppedSamples: 0, discardedQueries: 0, invalidatedSamples: 0};
    let active = null, invalid = null, finalResult = null;
    let reason = supported ? null : ext ? 'The GPU elapsed-time counter has zero bits.' : `${extensionName} is unavailable.`;
    if (supported) gl.getParameter(ext.GPU_DISJOINT_EXT); // Clear clock changes from before this trial.

    function releaseQueries() {
      if (active && !gl.isContextLost()) gl.endQuery(ext.TIME_ELAPSED_EXT);
      active = null;
      for (const query of owned) gl.deleteQuery(query);
      owned.clear(); idle.length = 0; pending.length = 0;
    }

    function invalidate(status, message) {
      if (finalResult || invalid) return;
      invalid = status; reason = message;
      counts.invalidatedSamples += samplesMs.length; samplesMs.length = 0;
      counts.discardedQueries += pending.length + (active ? 1 : 0);
      releaseQueries();
    }

    function poll() {
      if (finalResult || !supported || invalid) return;
      if (gl.isContextLost()) { invalidate('context-lost', 'The WebGL context was lost during GPU timing.'); return; }
      if (gl.getParameter(ext.GPU_DISJOINT_EXT)) {
        invalidate('disjoint', 'A GPU clock disjoint invalidated all GPU samples for this trial.'); return;
      }
      for (let i = 0; i < pending.length;) {
        const query = pending[i];
        if (!gl.getQueryParameter(query, gl.QUERY_RESULT_AVAILABLE)) { i++; continue; }
        const nanoseconds = gl.getQueryParameter(query, gl.QUERY_RESULT);
        pending.splice(i, 1); counts.completedQueries++;
        if (!Number.isSafeInteger(nanoseconds) || nanoseconds < 0) {
          invalidate('error', 'The GPU query returned an invalid elapsed time.'); return;
        }
        if (samplesMs.length < 100000) samplesMs.push(nanoseconds / 1e6);
        else counts.droppedSamples++;
        idle.push(query);
      }
    }

    function begin() {
      if (finalResult) throw new Error('GPU timer is already finished');
      counts.attemptedFrames++;
      if (!supported || invalid) { counts.droppedFrames++; return false; }
      if (active) {
        invalidate('error', 'A GPU elapsed-time query was still active at frame start.');
        counts.droppedFrames++; return false;
      }
      let query = idle.pop();
      if (!query && owned.size < maxPendingQueries) {
        query = gl.createQuery();
        if (!query) {
          invalidate('error', 'Cannot allocate a GPU elapsed-time query.'); counts.droppedFrames++; return false;
        }
        owned.add(query);
      }
      if (!query) { counts.droppedFrames++; counts.droppedPoolFull++; return false; }
      gl.beginQuery(ext.TIME_ELAPSED_EXT, query);
      active = query; counts.submittedQueries++;
      return true;
    }

    function end() {
      if (!active) return;
      gl.endQuery(ext.TIME_ELAPSED_EXT); pending.push(active); active = null;
    }

    function snapshot(final = false) {
      if (finalResult) return finalResult;
      poll();
      const pendingQueries = pending.length + (active ? 1 : 0);
      const status = invalid || (!supported ? 'unsupported' : samplesMs.length ? 'measured' : 'no-samples');
      const currentReason = status === 'no-samples' ? 'No completed GPU query result is available.' : status === 'measured' ? null : reason;
      return {samplesMs: samplesMs.slice(), info: {
        extension: extensionName, supported, status, reason: currentReason, ...counts, sampleCount: samplesMs.length,
        ...(final ? {pendingAtFinish: pendingQueries} : {}), pendingQueries, maxPendingQueries,
        sampleLimit: 100000,
        scope: 'Clear, background and protein draw commands; excludes geometry preparation, uploads and presentation.'
      }};
    }

    function finish() {
      if (finalResult) return finalResult;
      finalResult = snapshot(true);
      releaseQueries();
      return finalResult;
    }

    return {begin, end, poll, snapshot, finish,
      contextLost: () => invalidate('context-lost', 'The WebGL context was lost during GPU timing.')};
  }

  globalThis.FAHGpuTimer = {createGpuTimer};
})();
