# Windows Vulkan host

The host builds a native `FAHSaver.scr` and an identical `fah-preview.exe`
standalone harness. It does not install or activate the screensaver.
Keep the `shaders` directory beside the executable.

Requirements: Windows 10 version 1803 or later, a C++17 compiler with Windows
SDK, and Vulkan headers, loader/import library and `glslangValidator` at build
time. At runtime, the GPU driver must provide Vulkan graphics and presentation.
The root build must define `fah_core` and `fah_source` before adding this directory.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Locate `fah-preview.exe` in the build output, then run a recorded model:

```powershell
.\fah-preview.exe --capture C:\private\protein.json --fps 60 --seconds 30 --style 2 --playback loop --background sky --metrics C:\private\vulkan.json
```

Or select a live FAH endpoint reachable through the private LAN or Tailscale:

```powershell
.\fah-preview.exe --source ws://HOST:7396/api/websocket --fps 60 --style cycle --playback latest --background solid --colour 101820
```

`--unit ID` selects a particular current WU. Fixed styles are `1`, `2` and `3`;
`cycle` and `random` change style every ten seconds. Loop playback advances
snapshots at ten snapshots per second. Display frame rate is independent.
`--fps` accepts integers from 1 to 240. No model is substituted when data is
unavailable; the host displays connection or configuration status instead.
Live loop retention fails explicitly at 128 MiB of coordinates or 10,000
snapshots. Latest mode retains one snapshot. A disconnection clears the model.

`--fullscreen true` runs the harness through the same full-screen host and input
handlers as `/s`. It defaults to false and is not saved in preferences. Use
`--seconds 5` for a bounded full-screen smoke test. Explicit durations must be
positive and no greater than 3600 seconds; omitting duration runs until exit.
`--width` and `--height` select physical client pixels for windowed comparisons,
defaulting to 1280 × 720. Both accept whole values from 64 to 4096 and are not
saved. Full-screen and preview sizes still come from Windows or the preview
parent. Metrics record the actual client width and height.

Native screensaver entry points:

- `/s` runs full-screen and exits on keyboard or mouse activity.
- `/p HWND` or `/p:HWND` embeds the preview in a supplied parent window.
- `/c` or `/c:HWND` opens configuration. Settings are stored under the current
  user's `Software\FAHSaver` registry key.

The initial full-screen host uses one surface across the virtual desktop. Mixed
refresh rates and independent per-monitor scenes need further host work.
Display changes update that surface to the current virtual-desktop bounds.

Rendering reuses shared mesh buffers. Instance buffers change only when the
displayed snapshot or style changes. A waitable frame timer caps rendering
without a busy loop. The prototype uses host-visible buffers; measure their
cost on discrete GPUs before selecting the final memory-upload strategy.

Metrics contain submission count/rate, process CPU time, CPU draw-call duration,
completed GPU timestamp-query duration where supported, and peak working set.
Submission count is not a measurement of displayed fps. Presentation timestamps,
GPU memory and power are unmeasured. The renderer uses FIFO presentation and
one in-flight frame. Record adapter and actual display configuration alongside
each comparison result.

`--metrics` also enables aggregate CPU diagnostics. The result separates the
UI/main thread's CPU time from whole-process CPU time. It counts pump iterations,
dispatched messages, render calls and timer/input-queue wakes. The input queue
includes GUI and posted messages, not only physical keyboard or mouse input.
Phase totals record wall time and main-thread CPU time for scene build/upload,
draw fence/query wait, image acquisition, command recording/submission and
presentation. The scene phase includes its upload fence wait and temporary-data
cleanup. Sampling overhead is included; there is no per-frame logging. CPU time
uses `GetThreadTimes`, whose reported units do not guarantee scheduling precision.
These diagnostics and Vulkan timestamp queries are disabled without `--metrics`.

Timing scopes are separate. `active_interval` stops at entry to `WM_DESTROY`,
before source stop or renderer destruction. `shutdown_interval` covers that
source/device shutdown. `lifecycle_interval` continues from the same post-setup
start until metrics collection, including shutdown. The legacy top-level elapsed,
CPU and rate fields retain that lifecycle scope; their labels do not establish
active pacing. Initial renderer/capture setup and result writing are excluded.
`submission_call_cadence` uses first/last successful `vkQueueSubmit` return
timestamps and `(count - 1) / interval`. It excludes setup and teardown and
measures submit calls, not displayed frames. One submission has no cadence rate.

Validate a completed capture trial with:

```powershell
python platform\windows\tests\check_diagnostics.py C:\private\vulkan.json
```

Exported Zig-built native bundles must include the nlohmann JSON MIT licence and
the static-runtime notices for libc++, libc++abi, libunwind and the bundled
MinGW runtime. Those toolchain-specific notices accompany the binary bundle;
they are not additional application dependencies.

Local checks compile the four GLSL shaders to SPIR-V, validate their output and
run portable option and projection/winding tests. The complete host, core and
WinHTTP source also cross-compile through CMake/Ninja to an x64 Windows GUI
executable using Zig and its bundled MinGW libraries. A native Windows SDK build,
actual culling image comparison and `.scr` lifecycle tests must still run on
Windows before this host is considered verified.

Implementation references:

- [Khronos swapchain semaphore reuse](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)
- [Microsoft high-resolution waitable timers](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw)
- [Microsoft screen saver integration](https://learn.microsoft.com/en-us/windows/win32/lwef/screen-saver-library)
