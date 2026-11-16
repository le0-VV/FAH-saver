# Optional WebGL GPU timing

Timed trials use `EXT_disjoint_timer_query_webgl2` when the browser exposes an
elapsed-time counter. Preview rendering and warm-up frames do not submit timer
queries. The extension is optional; WebKit may not expose it. Availability must
be checked in each runtime rather than inferred from another browser.

Each query covers clear, background and protein drawing. It excludes geometry
preparation, instance uploads before drawing, browser compositing and display
presentation. Query results are polled on later animation callbacks. The timer
reuses at most eight query objects. If all eight are pending, that frame's GPU
sample is dropped without delaying drawing. It never waits for query results.
This follows the [Khronos extension contract](https://registry.khronos.org/webgl/extensions/EXT_disjoint_timer_query_webgl2/).

The result has these fields:

- `gpuTimeMs`: mean, median, p95 and p99 milliseconds from valid completed samples,
  or `null` when GPU timing is unmeasured.
- `gpuTiming.status`: `measured`, `unsupported`, `no-samples`, `disjoint`,
  `context-lost` or `error`. `reason` explains an unmeasured result.
- `attemptedFrames`, `submittedQueries`, `completedQueries` and `sampleCount`
  describe sample coverage. `completedQueries` can include samples later
  invalidated; only `sampleCount` contributes to `gpuTimeMs`.
- `droppedFrames` includes unsupported/invalid timing, pool saturation and query
  allocation failures. `droppedPoolFull` identifies pool saturation separately.
- `pendingAtFinish` counts query results still unavailable at the final poll.
  They are deleted and excluded from the samples. No result is invented for them.
- `discardedQueries` counts queries discarded during invalidation.
  `invalidatedSamples` counts previously collected samples removed by it.

A disjoint event or context loss invalidates every GPU sample in the affected
trial. It cannot yield a zero GPU duration in place of missing data. Queries are
released after completion, cancellation, context loss or page closure. Query
instrumentation adds overhead and must be included when assessing trial costs.

RequestAnimationFrame metrics still describe callback cadence. GPU elapsed time
does not establish display presentation or power use. Run the mock lifetime and
failure tests with `node benchmark/web/self-test.mjs`; an actual runtime trial is
required to establish extension availability and collect real GPU samples.
