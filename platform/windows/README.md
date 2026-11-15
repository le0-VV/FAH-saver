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
Live loop retention fails explicitly at 128 MiB of coordinates or 100,000
snapshots. Latest mode retains one snapshot. A disconnection clears the model.

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
