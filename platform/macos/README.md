# macOS native prototype

The CMake subdirectory builds three targets:

- `fah_macos_preview`: a native Metal preview and measurement harness.
- `FAHSaver`: a real ScreenSaverView module, packaged as `FAHSaver.saver`.
- `fah_macos_saver_smoke`: a hidden module host for lifecycle smoke checks.

The parent build supplies `fah_core` and `fah_source`. All targets use the system
AppKit, Metal, MetalKit and QuartzCore frameworks. The saver and module host also
use ScreenSaver. The minimum deployment target is macOS 10.15. Apple silicon is
the tested build architecture; Intel has not been tested.

## Preview and measurement

Example commands, using paths from the parent build directory:

```sh
./platform/macos/fah_macos_preview --capture /path/to/capture.json \
  --style 2 --playback loop --fps 60 --seconds 10 --background sky

./platform/macos/fah_macos_preview --source ws://127.0.0.1:7396/api/websocket \
  --playback latest --fps 60 --seconds 10

./platform/macos/fah_macos_preview --capture /path/to/capture.json \
  --style 1 --fps 60 --seconds 1 --width 1280 --height 720 --offscreen \
  --background solid --colour 0.02,0.025,0.035 --screenshot /tmp/protein.png
```

Use `--style cycle` or `--style random` for timed style changes. The provisional
style interval is 10 seconds; loop playback advances a snapshot every 0.1 seconds.
The user frame-rate limit does not alter snapshot cadence. `--unit ID` selects a
specific WU for a live source. The source emits an explicit error if selection is
ambiguous. The harness waits up to 30 seconds for its first usable live snapshot.

Output JSON distinguishes CPU encoding time, GPU command-buffer time and actual
drawable presentation intervals. Offscreen mode has no presentation timestamps,
so its delivered display fps is zero and its output states this limitation.
Offscreen tests can check rendering and GPU cost, but cannot establish display
pacing. Live latest mode retains only its latest received snapshot. Capture-file
mode keeps the selected recording unchanged so renderers can compare the same
fixture. Raw donor or account state is not written to metrics.

Drawable width and height refer to physical pixels. The defaults are 1024×768.
Metal uses one sample per pixel, opaque shading and no face culling. Geometry is
instanced and uploaded once. Instance data changes only on snapshot or style
updates. Per-display-frame work updates camera uniforms and submits draw calls.

## Screensaver options and checks

The native options sheet selects a live source or capture file, optional WU ID,
latest or loop playback, a fixed/cycling/random style, a 1–240 fps limit and a sky
or solid-colour background. Options use ScreenSaverDefaults. Starting and stopping
the saver starts and stops its native source; a source reset clears the model.
Missing data and connection errors appear as text over the background.

```sh
python3 /path/to/source/platform/macos/test_cli.py ./platform/macos/fah_macos_preview

./platform/macos/fah_macos_saver_smoke \
  ./platform/macos/FAHSaver.saver /path/to/capture.json
```

The smoke host loads the actual module, checks its principal class and options
sheet, then runs ten start/stop cycles. It does not install or activate the saver,
change user defaults, lock the computer or test ScreenSaverEngine itself.

When a restricted execution environment hides the Metal device, a GPU test must
run outside that environment. This is an execution-context requirement, not a
software-rendering fallback. Full-screen system-host behaviour, multiple displays,
lock/sleep recovery, distribution signing and Intel support remain unverified.
