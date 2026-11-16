# Windows WebView2 comparison host

This optional benchmark loads the same `benchmark/web` WebGL2 renderer as the
macOS comparison. It is not a production screensaver dependency. The installed
Microsoft Edge WebView2 Runtime is required; the host does not install a runtime
or switch to another browser when it is unavailable.

```powershell
.\fah-webview-benchmark.exe --capture C:\private\protein.json --html .\web\index.html --output C:\private\webview.json --width 1280 --height 720 --fps 60 --seconds 30 --warmup 0 --style 2 --playback loop --background sky
```

Width and height are physical **client** pixels. The WebView uses raw-pixel
bounds, and the result is rejected if the WebGL canvas dimensions differ.
`--fps` accepts whole values from 1 to 240. Durations range from 1 to 3600
seconds, with an optional 0–120 second warm-up. The timeout includes a further
30 seconds for browser startup. Fixed styles are `1`, `2`, `3`; `cycle` and
`random` use the shared page's ten-second interval. `--colour` accepts six RGB
hexadecimal digits for a solid background, for example `050609`.

The shared parser validates and canonicalises the capture before document-start
injection. The local classic HTML page receives `window.nativeCapture`. Its
existing result bridge forwards to WebView2 without changing the shared assets.
The host exits with a non-zero status for page errors, an unavailable runtime,
user closure or timeout. Runtime errors are written to the result file when
`--output` is supplied.

Each run creates a new profile under the current user's temporary directory.
This isolates its browser process group from normal Edge profiles. The host
closes the controller and waits up to five seconds for its browser to exit
before removing that profile. If exit cannot be verified or files remain in
use, the result records the retained directory. No unrelated browser process
is stopped.

## Measurement scope

The page's callback/submission metrics are preserved. Callback frequency does
not establish displayed fps. The host adds runtime version, browser PID,
startup-inclusive host CPU time and host peak working set.

When the installed runtime supports `ICoreWebView2Environment8`, the host also
samples that profile's process inventory every 500 ms and listens for inventory
changes. It retains process handles, including creation time, to distinguish PID
reuse and collect final CPU counts from processes that exit during the trial.
It reports observed browser process CPU, sampled working-set sums and sampled
private committed memory. Private committed memory is not resident memory.

`complete_process_cost_measured` is always false. Microsoft's inventory excludes
crashpad, short-lived processes can escape observation, and summed working sets
can count shared pages more than once. Failed process queries are explicit.
GPU memory, presentation timestamps and power are not measured by this host.

## Build and SDK provenance

The root build can add this directory after `fah_core`. Set `FAH_WEBVIEW2_SDK`
to the extracted official package to enable the optional executable. An empty
setting leaves the host disabled. The native x64 Windows SDK build uses:

```powershell
cmake -S . -B build -A x64 -DFAH_WEBVIEW2_SDK=C:\tools\webview2-sdk
cmake --build build --config Release --target fah_webview_benchmark
```

SDK checked on 16 November 2026:

- Package: `Microsoft.Web.WebView2` **1.0.4314.35**, stable.
- [Official NuGet package](https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.4314.35/microsoft.web.webview2.1.0.4314.35.nupkg).
- Package SHA256: `90778deb1b77e36292828b4b5833c9255c27533f40e0ddd55891ae718944edc4`.
- The package's `LICENSE.txt` permits redistribution with its copyright notice,
  conditions and disclaimer. The test bundle includes that complete notice as
  `WEBVIEW2-SDK-LICENSE.txt` beside the unmodified x64 `WebView2Loader.dll`.
- SDK headers and binaries remain outside Git. The optional host dynamically
  loads only the supplied loader beside its executable.

The temporary Zig/MinGW cross-build additionally needs `EventToken.h`, which is
already supplied by a native Windows SDK. The cross-build used the
[Microsoft win32metadata header](https://raw.githubusercontent.com/microsoft/win32metadata/main/generation/WinSDK/RecompiledIdlHeaders/winrt/EventToken.h),
SHA256 `5ba4a664c63d454572346598efd22b826c80c44ad9257eefbbe64c659fe56991`,
through `FAH_WEBVIEW2_SUPPORT_INCLUDE`. This header is not vendored in Git.

Verification status: portable option tests pass with warnings treated as errors.
The complete host also passes a Release CMake/Ninja x64 Windows cross-build
using Zig/MinGW and the official SDK. PE architecture and GUI subsystem are
checked. Actual Windows execution, runtime availability and result delivery
remain unverified until the coordinated PC trial completes.

## Primary references

- [Microsoft Win32 WebView2 setup](https://learn.microsoft.com/en-us/microsoft-edge/webview2/get-started/win32)
- [Process inventory and its crashpad exclusion](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2environment8)
- [Physical-pixel controller bounds](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2controller3)
- [User-data-folder lifecycle](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/user-data-folder)
