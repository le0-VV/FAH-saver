# Project details

Version **1.0.0** provides a native Metal screensaver for Apple-silicon Macs and
a native Vulkan screensaver with a per-user installer for Windows x64. The
default display limit is 24 fps. An independent WebGL comparison renderer is
included for development.

## Release limitations

- macOS downloads support Apple silicon; an Intel build has not been verified.
- The macOS saver is ad hoc signed and is not notarised. The Windows installer
  is unsigned. Platform trust approval may be required for downloaded binaries.
- The macOS legacy host can retain extra or inactive renderers after a Settings
  preview, causing continued GPU use and substantial memory use. This remains
  unresolved; 1.0.0 does not claim an optimal active or inactive footprint.
- Windows currently spans one surface across the virtual desktop rather than
  providing independent scenes per display.

See the [verification record](verification.md) for measured results and
their limits.

## Agreed requirements

| Area | Requirement |
| --- | --- |
| Initial platforms | macOS and Windows. Keep a future Linux version possible. |
| Connections | Local host, LAN and remote access through Tailscale. |
| Data source limit | Keep FAH's 25 MB visualisation cache limit for now. No companion collector. |
| Loop playback | Loop all available visualisation snapshots of the selected current WU, with a configurable interval (default 1 second). |
| Latest playback | Display the latest snapshot and update when another snapshot arrives. |
| Startup and offline display | Automatically cache one latest complete model for the selected source; show it while live data is unavailable. |
| Model styles | The current FAH viewer's three styles: space-filling, ball-and-stick and sticks. |
| Style selection | Keep one style, cycle through all three, or select styles at random. |
| Frame rate | Default to 24 fps with a user-configurable limit from 1 to 240 fps. |
| Backgrounds | Offer background options. Do not restrict the screensaver to black. |
| Performance | Prefer native networking and GPU rendering. Compare them against an embedded browser before selecting the stack. |

The display frame rate is separate from the rate at which FAH produces protein
snapshots. A 60 fps display can rotate the camera smoothly while displaying one
snapshot. It must not imply that FAH produces 60 new snapshots per second.

The screensaver connects to a live FAH source. It automatically saves the latest
complete topology and one snapshot locally. That model appears at startup and
stands in during connection loss, reconnects or a wait for a new WU's first
snapshot. A complete live snapshot replaces it. The cache does not extend FAH's
25 MB visualisation history limit. The first successful live connection fills
the cache; without a valid cache, the saver shows its normal connection status.

Follow Latest withholds historical startup frames until it reaches the selected
WU's published latest frame. The cached stand-in remains visible during catch-up.
Loop mode still receives the complete available history.

There is no manual capture-file selector in the screensaver options. The capture
and preview command-line tools remain available for development and repeatable
rendering checks.

## Rendering candidates

- macOS: a native `.saver` host with Metal rendering.
- Windows: a native `.scr` host with Vulkan rendering.
- Browser comparison: equivalent WebGL rendering in a system web view.

Use Vulkan on Windows to keep a future Linux renderer possible. Keep protein
data, playback and scene definitions independent of Windows APIs where practical.
A future Linux version will still need its own host and platform integration.
Linux is not part of the initial release.

The initial implementation keeps direct Metal on macOS and Vulkan on Windows.
The short hardware checks support this choice: native rendering meets the tested
cadence and uses less later-interval CPU than the observed browser comparison.
Browser hosts remain available for regression comparisons. A Vulkan-on-Metal
layer is not required. Sustained performance acceptance is still open.

The 17 November macOS WebView replacement experiment also favoured Metal. At
matched physical resolution and approximately 60 draws per second, the working
in-process WebView used 212–247 MiB for one Retina canvas versus 132–134 MiB for
Metal. With Retina and HD canvases together, it used 338–349 MiB versus
184–205 MiB. CPU use and sampled whole-GPU busy time were higher with WebView.
The user chose to restore Metal, and the normal module build selects Metal.
The complete experimental module is preserved on `agent/webview-saver-experiment`;
the independent Web renderer and matched comparison app remain available.
See the [comparison record](verification.md#macos-webview-replacement-experiment-on-17-november-2026)
for the method, compatibility failures and measurement limits.

## Build and run

Run the commands below from the repository root.

Build on macOS with Xcode command-line tools and CMake:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
node benchmark/web/self-test.mjs
```

The build creates `build/platform/macos/FAHSaver.saver`, a native preview and a
capture tool. It does not install or select the system screensaver. See the
[macOS instructions](../platform/macos/README.md) and
[Windows instructions](../platform/windows/README.md) for platform-specific runs.
Windows releases provide a per-user Setup.exe that handles installation,
registration, updates and uninstallation. The downloaded setup file can be
deleted after installation. See the [installer instructions](../platform/windows/installer/README.md).
Windows development builds need a C++17 toolchain, Windows headers and Vulkan
SDK tools. Windows cancellation tests also use Python 3. Browser data tests use
Node.js; the screensavers do not need Node.js.

For a private connection to a default FAH client that listens only on loopback,
forward its API through an existing SSH connection. The folding host can be
reached by its LAN address or Tailscale address:

```sh
ssh -N -L 127.0.0.1:17396:127.0.0.1:7396 USER@FOLDING_HOST
```

Set the screensaver source to `ws://127.0.0.1:17396/api/websocket`. Keep the tunnel
running while viewing. This route needs existing SSH access; it does not change
FAH's listener. A direct private LAN or Tailscale connection is also supported
when FAH is already configured to accept that connection.
See the [direct-access setup](direct-access.md) for listener, access-rule
and Windows firewall requirements. macOS 0.1.2 and later use explicit direct source routing and
reports HTTP upgrade errors explicitly.

Capture only protein topology and coordinates for a repeatable rendering test:

```sh
mkdir -p captures
build/fah_capture --url ws://127.0.0.1:17396/api/websocket \
  --output captures/protein.json --frames 8 --seconds 20
build/platform/macos/fah_macos_preview --capture captures/protein.json \
  --style 2 --playback loop --fps 60 --seconds 10
```

The capture tool refuses to overwrite an existing file. `captures/`, `results/`
and build directories are excluded from Git. Live sources issue visualisation
requests only. Source selection uses the first running WU unless a WU ID is
specified. A fixed WU selection never silently switches to another WU.

## Visual reference and prototype defaults

The current FAH viewer creates a sky and grid scene. It does not use a plain
black scene as its only background. Use that scene as the visual reference.
The implemented options are light and dark sky/grid approximations and a solid
colour chosen by the user. The atmospheric FAH sky shader is not reproduced.
Light sky/grid remains the default. The options panel shows the loaded saver
version in its lower-left corner.

Camera rotation is 0.2 radians per second. Loop playback advances a snapshot every
1 second by default; its interval accepts finite values of at least 0.05 seconds,
with no imposed upper limit. The options field accepts decimal digits and one
decimal point, with inline validation instead of a modal alert. Latest mode updates
when FAH supplies a snapshot and ignores the loop interval. Styles change every
ten seconds in cycle or random mode. All three model styles frame
the bonded structure consistently; isolated atoms remain in the scene but can
lie outside the camera view. See the [capture conventions](capture-format.md).

FAH's web UI has dark and light CSS themes, while its 3D sky uses a separate
animated atmospheric shader. These native background presets are inexpensive
approximations, rather than exact copies of that sky. See the upstream
[theme](https://github.com/FoldingAtHome/fah-web-client-bastet/blob/c54888af2a261d79270a390f289a4972965cedb4/src/dark.styl)
and [sky](https://github.com/FoldingAtHome/fah-web-client-bastet/blob/c54888af2a261d79270a390f289a4972965cedb4/src/viewer/Sky.js)
sources. Historical browser comparison runs use a 0.1-second loop interval; set
`--snapshot-interval 0.1` on native previews when repeating that comparison.

## Verified upstream behaviour

The protocol was checked against upstream source and the installed Windows FAH
8.5.6 client. Native macOS capture succeeded through both LAN and Tailscale SSH
tunnels. The tested real WU contained 11,412 atoms and 21,426 bonds. The Tailscale
test was on the same site; off-site operation has not been verified.

- The v8 client accepts a `viz` request through its WebSocket API. A request with
  frame cursor zero sends topology followed by its available cached frames.
  Subsequent frames are pushed as they arrive. A frame cursor supports resuming.
- A fresh latest-mode connection can still receive existing history to obtain
  topology. Do not promise a latest-only initial transfer with this API.
- Visualisation snapshots are not every simulation timestep. The client limits
  collected topology and frame JSON to 25,000,000 bytes. Some WUs have no usable
  visualisation data.
- WU cleanup removes the working files. Viewing a completed WU would require a
  cache captured before cleanup; completed-WU archives are not an agreed feature.
- The same API accepts folding-control and configuration commands. Use the
  private LAN or Tailscale path. The screensaver must issue only the requests
  needed to select a WU and receive visualisation data.

This tested v8 data flow needs no additional helper on the folding host.

## Investigation sequence

The [performance comparison plan](performance-plan.md) defines the trial
conditions, measurements and proposed acceptance criteria.

1. Identify the installed FAH version. Capture a real topology and multiple
   snapshots without changing or interrupting folding.
2. Use identical recorded data to compare native and browser renderers at
   matched visual quality, drawable resolution and frame-rate limits, including
   the 60 fps shipping target.
3. Measure resource use and frame pacing on both platforms. Include the effect
   on FAH throughput when the screensaver and folding share a computer.
4. Verify preview, full-screen, multiple displays, stop/start, lock, sleep and
   reconnect behaviour in the real screensaver hosts.
5. Record measurements and select the implementation stack. Do not select a
   winner from CPU utilisation or a desktop preview alone.

Connection loss, missing visualisation data and a changed WU are explicit
states. A complete cached/latest model remains visible during a disconnection
or pending WU topology; connection status appears when no model is available.
The prototypes do not silently change the selected source, renderer or quality.

## Evidence

Research checked on 16 November 2026. Source links are pinned where possible.

- [FAH visualisation delivery and command handling](https://github.com/FoldingAtHome/fah-client-bastet/blob/7c4363ae7981ece320047e26200fc244442a91bf/src/fah/client/Remote.cpp)
- [FAH visualisation collection and WU cleanup](https://github.com/FoldingAtHome/fah-client-bastet/blob/7c4363ae7981ece320047e26200fc244442a91bf/src/fah/client/Unit.cpp)
- [FAH viewer styles, sky and grid](https://github.com/FoldingAtHome/fah-web-client-bastet/blob/c54888af2a261d79270a390f289a4972965cedb4/src/Visualization.vue)
- [Apple Screen Saver framework](https://developer.apple.com/documentation/screensaver)
- [Microsoft screen saver integration](https://learn.microsoft.com/en-us/windows/win32/lwef/screen-saver-library)
