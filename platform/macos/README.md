# macOS native prototype

The CMake subdirectory builds these targets:

- `fah_macos_preview`: a native Metal preview and measurement harness.
- `FAHSaver`: a real ScreenSaverView module, packaged as `FAHSaver.saver`.
- `fah_macos_saver_smoke`: a hidden module host for lifecycle smoke checks.
- `fah_macos_render_options_tests`: offscreen GPU checks when testing is enabled.
- `fah_macos_options_lifecycle_tests`: repeated sheet Save/Cancel/close/reopen checks.
- `fah_macos_cache_lifecycle_tests`: cached startup and live/offline model handover checks.
- `fah_macos_presentation_lifecycle_tests`: retained-view hide/detach cleanup and resume checks.
- `fah_macos_session_lifecycle_tests`: missed host stop, stale restart and multi-display session checks.

The parent build supplies `fah_core` and `fah_source`. All targets use the system
AppKit, Metal, MetalKit and QuartzCore frameworks. The saver and module host also
use ScreenSaver. The prototype deployment target is macOS 11.0. Apple silicon is
the tested build architecture; Intel has not been tested.

## Preview and measurement

Example commands, using paths from the parent build directory:

```sh
./platform/macos/fah_macos_preview --capture /path/to/capture.json \
  --style 2 --playback loop --fps 60 --warmup 30 --seconds 120 --background sky

./platform/macos/fah_macos_preview --source ws://127.0.0.1:7396/api/websocket \
  --playback latest --fps 60 --seconds 10

./platform/macos/fah_macos_preview --capture /path/to/capture.json \
  --style 1 --fps 60 --seconds 1 --width 1280 --height 720 --offscreen \
  --background solid --colour 0.02,0.025,0.035 --screenshot /tmp/protein.png
```

Use `--style cycle` or `--style random` for timed style changes. The provisional
style interval is 10 seconds. Loop playback defaults to a snapshot every 1
second; `--snapshot-interval SECONDS` accepts finite values of at least 0.05
seconds, with no upper limit. Latest mode ignores
the loop clock. Use `--snapshot-interval 0.1` to repeat the historical browser
comparison with its original cadence.
The user frame-rate limit does not alter snapshot cadence. `--unit ID` selects a
specific WU for a live source. Without an ID, the source selects the first running
WU and reports when none is available. The harness waits up to 30 seconds for its
first usable live snapshot.
Live latest mode enables the source adapter's latest-only delivery policy:
published history is validated but not displayed before the reported tail arrives. Loop
mode still receives the available history. The developer capture tool keeps its
full-history delivery policy.

Output JSON distinguishes CPU encoding time, GPU command-buffer time and actual
drawable presentation intervals. Offscreen mode has no presentation timestamps,
so its delivered display fps is zero and its output states this limitation.
Offscreen tests can check rendering and GPU cost, but cannot establish display
pacing. Live latest mode retains only its latest received snapshot. Capture-file
mode keeps the selected recording unchanged so renderers can compare the same
fixture. Raw donor or account state is not written to metrics.

Use `--warmup 0..120` before the measured duration. Warm-up defaults to zero.
The camera and snapshot clocks continue across the measurement boundary. Only
submissions and presentations from the measured phase enter its metrics.

Drawable width and height refer to physical pixels. The defaults are 1024×768.
Metal uses one sample per pixel, opaque shading and back-face culling. Geometry is
instanced and uploaded once. Instance data changes only on snapshot or style
updates. Per-display-frame work updates camera uniforms and submits draw calls.
Apple-family GPUs use memoryless D32 depth: the pass clears and discards depth,
so no full-resolution depth contents must survive the pass. Other GPUs use
private D32 depth. Colour resolution and depth precision are unchanged.
Aggregate metrics report `depth_storage_policy` and the device's
`metal_current_allocated_bytes`; this device-wide value must not be summed across
renderer instances and is not a complete process-footprint measurement.

## Screensaver options and checks

The native options sheet selects a live source URL, optional WU ID,
latest or loop playback, a fixed/cycling/random style, a 1–240 fps limit (default 24 fps) and a light
sky/grid, dark sky/grid or solid-colour background. The loop interval field is
visible only in loop mode and defaults to 1 second for new settings. Existing
explicit values are preserved. The field accepts ASCII digits and one decimal
point, including unfinished text while editing. Its native field editor rejects
invalid keyboard and paste replacements before they change the text. Invalid
values keep the panel open with a small message directly below the offending
field; lower rows move down temporarily and no modal alert is used. `--background sky|dark|solid`
selects these backgrounds in the preview. The panel's lower-left corner shows
the version and build of the loaded saver bundle. Options use ScreenSaverDefaults.
Starting and stopping the saver starts and stops its native source. The retired
capture-file controls and mode no longer affect the screensaver. Developer
preview and capture tools still accept recordings for repeatable tests.

The saver automatically keeps the latest complete topology and one snapshot in
`NSCachesDirectory/org.fahsaver.screensaver/latest-model.cache`. In the legacy
host this directory is inside its sandbox container. The cache is bound to the
configured source URL and optional requested WU, without storing those strings.
It is read before the source starts. Pending topology, reconnects and connection
failures leave that model visible; the first complete live snapshot replaces it.
Loop history is retained only for the live session. On loss of the connection,
the latest received snapshot becomes the stand-in rather than looping stale
history. If no valid cache exists, the saver shows the normal connection status
until its first live snapshot arrives.

Disk writes run on one coalescing cache worker, not on display-frame callbacks.
Cache failures are logged and do not stop live reception or discard a valid
in-memory model. Renderer/configuration errors still appear over the view.
Connecting and retry messages are hidden while a valid model is available.

Actual hiding (including a hidden ancestor) or removal from an existing
superview suspends the saver even if the host omits `stopAnimation`. A public
`NSWindowWillCloseNotification` for the view's actual window performs an explicit
stop; closing a retained window need not hide or detach its content views. The
observer changes with the window and does not gate initial windowless hosting.
Suspension
stops its timer/source/cache worker, retains only the latest model and releases
the MTKView/CAMetalLayer presentation owner. Showing or reattaching a previously
running view recreates that owner once and resumes from the cached model. An
explicit stop cancels automatic resume. Initial windowless hosting remains
supported; neither a missing window nor window occlusion is used as a stop test.
The presentation regression retains the saver across 20 hide/detach/resume
cycles and checks released weak view/layer references, stopped callbacks and
deferred hidden starts, plus retained-view window closure. These public lifecycle checks do not establish that
every macOS host retention path emits the required hide/removal events.

The legacy screensaver host can retain full-screen views after dismissal without
calling stop or changing their visible geometry. Its non-preview views also
observe workspace application activation. After a view observes Login Window as
the foreground app during animation, activation of another app performs an
explicit stop. The notification's application identity is used because a separate
frontmost-app query can briefly lag behind the event. Later stale start requests
wait for the next Login Window activation. That activation resumes a view only
when the host requested a new start; it does not restart all retained views.
An explicit stop cancels a pending restart. Preview views and other hosts retain
their normal lifecycle. The guard does not block an initial windowless start or
use window occlusion as proof that a remote view is visible.

The programmatic options window disables AppKit's automatic release-on-close;
ARC owns its lifetime. The lifecycle test uses transparent, non-interactive
windows and an isolated preference domain for ten saves and ten cancels. It
checks sheet completion, cleared attachments, saved values and discarded edits.

```sh
python3 /path/to/source/platform/macos/test_cli.py ./platform/macos/fah_macos_preview

./platform/macos/fah_macos_saver_smoke \
  ./platform/macos/FAHSaver.saver /path/to/capture.json
```

The smoke host loads the actual module, checks its principal class, version label
and loop-only interval controls, then runs ten start/stop cycles with two independent views of different
sizes. Stopping one view must leave the other animating. This hidden check does
not submit display drawables or establish multi-display presentation.
It uses a volatile `NSArgumentDomain` preference override, an injected fixture
source and a temporary cache. The fixture source does not open a socket. Normal
engine construction still uses ScreenSaverDefaults and the native live source.
Persistent settings must compare
unchanged before and after each check, and no source callbacks may continue after
stop. It does not install or activate the system saver, lock the computer or test
ScreenSaverEngine itself.

The cache lifecycle check loads the actual module with an isolated cache and a
controlled source. It checks model availability before source startup, offline
retention, pending WU topology, matching-frame handover, latest-only persistence,
source-selection isolation, corrupt-cache replacement and ignored callbacks
after stop. It does not activate the system saver or contact a folding host.

For an explicitly authorised brief full-screen check:

```sh
./platform/macos/fah_macos_saver_smoke ./platform/macos/FAHSaver.saver \
  /path/to/capture.json --fullscreen --screenshot /tmp/fullscreen-saver.png

./platform/macos/fah_macos_saver_smoke ./platform/macos/FAHSaver.saver \
  /path/to/capture.json --fullscreen --test-input
```

The first form exits after five seconds or normal keyboard/mouse input. Its
first second warms up the view; the remaining interval collects actual drawable
presentation timestamps, GPU times and the physical drawable dimensions. The
helper lets ScreenSaverView own its periodic animation timer and reports all
callbacks from the real view. It does not add a second drawing timer. Submitted
frame counts must remain unchanged during the post-stop event drain. The
second posts Escape through the app's event queue after 1.5 seconds to exercise
its input-exit path. Output identifies that synthetic input explicitly. Window
capture is optional and reports unavailable recording permissions as an error
field. These remain module-host checks, distinct from system engine verification.

When a restricted execution environment hides the Metal device, a GPU test must
run outside that environment. This is an execution-context requirement, not a
software-rendering fallback. Actual ScreenSaverEngine behaviour, multiple displays,
lock/sleep recovery, distribution signing and Intel support remain unverified.
