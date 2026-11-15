# Browser comparison renderer

This dependency-free WebGL 2 renderer uses the same instanced sphere and cylinder
meshes, atom radii, camera fit and simple lighting as the native candidates.
It is a benchmark candidate, not the selected shipping screensaver.

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
GPU duration and power are explicitly unmeasured. The shipping 60 fps target
therefore requires OS presentation measurements and the full host/process costs
from the [comparison plan](../docs/performance-plan.md).

Capture input is limited to 32 MiB, 200000 atoms, 1000000 bonds and 10000 frames,
with a decoded budget of 2000000 positions. Invalid atom indices, frame sizes,
coordinate types and non-finite coordinates stop loading with an explicit error.
