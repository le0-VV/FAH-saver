# Browser comparison renderer

This dependency-free WebGL 2 renderer uses the same instanced sphere and cylinder
meshes, atom radii, camera fit and simple lighting as the native candidates.
It is a benchmark candidate, not the selected shipping screensaver.

The drawing engine in `web/renderer.js` is shared with the local screensaver
runtime. `web/saver.html` has no controls or network source; its native bridge
contract is documented in [RUNTIME.md](web/RUNTIME.md). The normal production
module remains Metal after the measured WebView experiment.

When bonds exist, framing uses every atom referenced by a bond, across all bonded
components. Otherwise it uses all atoms. Every atom is still submitted at its
original relative position; framing does not wrap, filter or distort coordinates.

From the repository root, serve files locally:

```sh
python3 -m http.server 8000 --bind 127.0.0.1
```

Open `http://127.0.0.1:8000/benchmark/web/`, then select or drop a capture JSON
file. The capture format is `{"topology": <FAH topology>, "frames": [<FAH frame>, ...]}`.
No protein loads automatically. Do not put account information in captures.

For unattended runs, use these query parameters:

```text
?capture=/captures/current.json&style=1&fps=60&seconds=120&warmup=30&background=sky&playback=loop&benchmark=1
```

| Parameter | Values |
| --- | --- |
| `capture` | URL to a capture; fetches must remain permitted by the browser. |
| `style` | `1`, `2`, `3`, `cycle` or `random`; default `1`. |
| `fps` | Integer limit from 1 to 240; default 60. |
| `seconds` | Trial length from 1 to 3600 seconds; default 30. |
| `warmup` | Warm-up from 0 to 120 seconds; default 0. |
| `background` | `sky` or `solid`; default `sky`. |
| `playback` | `latest` or `loop`; default `latest`. |
| `benchmark` | `1` hides controls and status during measurements. |
| `autostart` | `0` loads without starting timed measurements. |

Loop mode advances snapshots at 10 Hz. The camera rotates at 0.2 radians per
second. Cycle and random modes select a style every ten seconds. Random mode
uses a repeatable sequence so repeated trials have the same scene changes.
These timings are prototype settings, not agreed product defaults.

The sky and grid are a cheap approximation agreed for this comparison. They do
not reproduce the FAH viewer's full atmospheric shader. The default solid colour
is RGB `(0.02, 0.025, 0.035)`. Select another solid colour through the native colour
input. Reference-image checks must confirm matching scenes before timing.
All candidates use one sample per pixel, opaque materials and back-face culling
for the protein. The background disables culling. Camera rotation and style
intervals are matched between candidates.

On macOS, the CMake target `fah_web_benchmark` provides a WKWebView host:

```sh
build/fah_web_benchmark --capture captures/protein.json \
  --html "$PWD/benchmark/web/index.html" --style 2 --fps 60 --seconds 30 \
  --width 1280 --height 720 --playback latest --background sky \
  --output results/webgl.json
```

Width and height are physical drawable pixels. The host injects the capture and
does not start a local web server. Its CPU and memory metrics cover the host
only; separate WebKit processes remain explicitly unmeasured.

## Matched macOS app comparison

The newer `fah_engine_comparison` target produces
`build/benchmark/macos/FAHEngineComparison.app`. It hosts the actual Metal
renderer or native Web bridge, with one to three explicitly sized canvases.
Launch the app normally through LaunchServices/Finder so it has its own process
responsibility identity. It reads `Contents/Resources/Comparison.json`.
Use a private configuration and output directory. For example:

```json
{
  "engine": "web",
  "webBackend": "legacy",
  "webClock": "native",
  "capture": "/private/path/protein.json",
  "layouts": [{"width": 2880, "height": 1800, "screen": 0}],
  "warmupSeconds": 10,
  "measureSeconds": 30,
  "cooldownSeconds": 10,
  "style": 1,
  "fps": 60,
  "background": "solid",
  "colour": [0.02, 0.025, 0.035],
  "collectMetrics": false,
  "output": "/private/path/trial.json",
  "status": "/private/path/status.json"
}
```

`engine` is `metal` or `web`; the explicit Web backend is `wk` or `legacy`.
`webClock` is `raf` or `native`. No automatic backend fallback occurs. Native
clock mode coalesces elapsed-time requests and bounds JavaScript transactions,
not GPU completion. The production WebKit experiment required it because rAF
did not provide continuous animation in the tested host. Apple recommends
WKWebView rather than the deprecated in-process WebView API.

The app checks actual physical canvas dimensions and reports draw deltas. It
releases presentation owners and holds the app alive during stopped cooldown.
Normal-cost runs leave `collectMetrics` false. Separate timing runs enable
Metal presentation/GPU samples and optional WebGL GPU queries; unsupported
queries are explicit. Callback/submission cadence is not display presentation.

Start the external sampler before launching the app:

```sh
python3 tools/macos-render-sample.py --status /private/path/status.json \
  --output /private/path/processes.json --seconds 120 --gpu
```

The sampler measures the host plus its WebKit services using kernel process
responsibility, follows known process identities after exit and samples
simultaneous process footprints. Its responsibility query is benchmark-only
private macOS SPI and fails explicitly if unavailable. CPU counters are converted
from Mach ticks and checked against `getrusage` by `--self-test`. IORegistry GPU
utilisation covers the whole device and can include other workloads. Use native
hardware sampling when that counter is contaminated; mark per-process GPU time
unavailable on systems that do not expose it.

For fixed-time image checks, set `mode: "screenshot"`, `fixedTimeSeconds` and a
`screenshot` output path. Run the app's executable with
`--render-reference /absolute/config.json`. This path never orders or activates
windows and is explicitly labelled as hidden reference rendering. It cannot
serve as a process-responsibility or presentation benchmark. The config/help
checks run without creating UI.

## Native web-view harness

The HTML uses classic scripts, so a web view can load `index.html` with its
containing directory as the permitted local read location. Before document
scripts run, inject `window.nativeCapture = <capture JSON object>` through a
document-start user script. This avoids local-file fetch restrictions.

Alternatively call `window.fahBenchmark.setCapture(objectOrJSONString)` once
the page scripts are loaded. `window.fahBenchmark.ready` becomes true after
validation. `window.fahBenchmark.start()` starts a trial using the controls/query
settings. `window.fahBenchmark.stop()` ends a timed run with `cancelled` status.

At completion, `window.benchmarkResult` contains the result. The page also emits
a `fah-benchmark-result` DOM event, logs `FAH_BENCHMARK_RESULT` with JSON, and calls
`window.webkit.messageHandlers.benchmark.postMessage(result)` when that handler
exists. No network upload is performed. A hidden page, changed drawable size,
lost WebGL context or changed capture/settings invalidates the active trial.

## Verification and metric limits

Run the dependency-free data and geometry checks:

```sh
node benchmark/web/self-test.mjs
```

The page records requestAnimationFrame callback intervals, draw-callback cadence
and CPU submission time. These are **not display presentation timestamps**.
GPU duration is sampled asynchronously when the browser exposes a valid elapsed
timer. Unsupported counters, disjoint events, missing results and dropped samples
are explicit in `gpuTiming`; see [GPU timing](web/GPU-TIMING.md). Power remains
unmeasured. The shipping 60 fps target therefore requires OS presentation measurements and the full host/process costs
from the [comparison plan](../docs/performance-plan.md).

Capture input is limited to 32 MiB, 200000 atoms, 1000000 bonds and 10000 frames,
with a decoded budget of 2000000 positions. Invalid atom indices, frame sizes,
coordinate types and non-finite coordinates stop loading with an explicit error.
