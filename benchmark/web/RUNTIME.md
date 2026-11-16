# Local screensaver runtime

`saver.html` is the production page. It has no controls or network source. Bundle
it with `saver.css`, `capture.js`, `scene.js`, `gpu-timer.js`, `renderer.js` and
`saver.js` in the same local directory. The native host supplies validated model
data and owns connection/cache policy. The Content Security Policy permits
local scripts/styles and rejects network connections and other resources.

The page posts `renderer` script messages through WebKit: `{type: 'ready'}` once
the API and WebGL context exist, `{type: 'error', message}` on an actual failure,
and `{type: 'frame', draws, width, height}` on the first model draw of each start.
There is no per-frame message stream.

`window.fahSaver` provides:

- `configure(options)`: accepts `fps` (1–240), numeric `style` (1–3), `styleMode`
  (`fixed`, `cycle`, `random`), `playback` (`latest`, `loop`), `background`
  (`sky`, `dark`, `solid`), `colour` (three components from zero to one),
  `snapshotInterval` (finite, at least 0.05 seconds), `styleInterval` (positive)
  boolean `collectMetrics`, and internal `frameDriver` (`raf` or `native`). Defaults are 60 fps, style 1, latest, light sky,
  one second per snapshot, ten seconds per style, and metrics disabled.
- `setCapture(rawCapture)`: replaces a complete topology/model using the normal
  FAH capture format. `setCapture(null)` clears the old model immediately.
- `appendFrames(rawFrames)`: validates the whole batch before appending in loop
  mode. Latest mode retains its tail only.
- `replaceLatest(rawFrame)`: keeps one new frame with the existing topology.
- `start()` / `stop()`: start is idempotent; stop cancels scheduled frames while
  retaining the model. A later start begins a new animation clock.
- `dispose()`: stops callbacks and deletes GPU resources/context. The native
  host must dispose before dropping the web view.
- `advanceTo(seconds)`: in native-clock mode, renders an eligible frame from
  monotonic, non-negative host elapsed time. Stopped/empty sessions return false.
  The configured frame limit still applies. Native-clock mode never requests rAF.
  Select the driver while stopped; the host must keep its own asynchronous
  request-generation guards when replacing a web view.
- `renderAt(seconds)`: draws one fixed-time frame, including while stopped or
  hidden, for image verification.
- `beginMeasurement()` / `metrics()`: explicit measurement enables bounded CPU,
  callback and optional GPU-query samples. Draws are lifetime counts;
  `measurementDraws` resets at measurement start. Measurement preserves camera
  time and the current model. Native-clock intervals are labelled separately
  from rAF intervals. Disabling `collectMetrics` clears measurement data.

Capture/frame updates preserve the rotation clock. Initial start draws the cached model synchronously at zero seconds before
scheduling animation. Without a model, it draws its background once and waits
for data. The first complete model arriving during a running session also draws
synchronously without resetting the clock. Live coordinates are limited
to 128 MiB and 100,000 snapshots. This is separate from the stricter developer
capture-file parser defaults. Latest mode does not retain a trajectory history.

`renderer.js` contains the geometry submission, shaders, backgrounds and camera
used by both this page and the benchmark controls. Benchmark GPU queries exclude
geometry preparation, uploads and presentation. Unsupported GPU counters are
reported explicitly. Callback cadence does not establish display presentation.

Run `node benchmark/web/self-test.mjs` from the repository root. Native WebKit
checks must additionally establish real shader compilation, image pixels,
physical resolution and asynchronous host teardown.

Runtime metrics also expose numeric-only `rafCallbackCount`,
`firstRafTimestampMs`, `firstRafPerformanceNowMs`, `epochMs` and `nextDueMs` for
clock diagnostics. The first rAF clock samples are recorded once. They add no
polling or per-frame script messages. No rAF is requested in native-clock mode.
