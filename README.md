# FAH Saver

A protein-model screensaver for macOS and Windows. The screensaver can display
the active Folding@home work unit (WU) from the same computer or another computer.

Status: runnable native prototypes and performance investigation. The macOS
Metal module, Windows Vulkan host, native FAH connection layer and WebGL
comparison renderer are implemented. Performance acceptance and system-host
validation are still in progress.

See the [verification record](docs/verification.md) for measured results and
their limits.

## Agreed requirements

| Area | Requirement |
| --- | --- |
| Initial platforms | macOS and Windows. Keep a future Linux version possible. |
| Connections | Local host, LAN and remote access through Tailscale. |
| Data source limit | Keep FAH's 25 MB visualisation cache limit for now. No companion collector. |
| Loop playback | Loop all available visualisation snapshots of the selected current WU. |
| Latest playback | Display the latest snapshot and update when another snapshot arrives. |
| Model styles | The current FAH viewer's three styles: space-filling, ball-and-stick and sticks. |
| Style selection | Keep one style, cycle through all three, or select styles at random. |
| Frame rate | Ship with a 60 fps target and a user-configurable frame-rate limit. |
| Backgrounds | Offer background options. Do not restrict the screensaver to black. |
| Performance | Prefer native networking and GPU rendering. Compare them against an embedded browser before selecting the stack. |

The display frame rate is separate from the rate at which FAH produces protein
snapshots. A 60 fps display can rotate the camera smoothly while displaying one
snapshot. It must not imply that FAH produces 60 new snapshots per second.

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

## Build and run

Build on macOS with Xcode command-line tools and CMake:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
node benchmark/web/self-test.mjs
```

The build creates `build/platform/macos/FAHSaver.saver`, a native preview and a
capture tool. It does not install or select the system screensaver. See the
[macOS instructions](platform/macos/README.md) and
[Windows instructions](platform/windows/README.md) for platform-specific runs.
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
The implemented options are a cheap sky/grid approximation and a solid colour
chosen by the user. The atmospheric FAH sky shader is not reproduced. Sky/grid
is the prototype default.

Camera rotation is 0.2 radians per second. Snapshot playback advances ten
snapshots per second in loop mode. Styles change every ten seconds in cycle or
random mode. These timings are prototype defaults. All three model styles frame
the bonded structure consistently; isolated atoms remain in the scene but can
lie outside the camera view. See the [capture conventions](docs/capture-format.md).

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

The [performance comparison plan](docs/performance-plan.md) defines the trial
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
states. A disconnection clears the displayed model and shows connection status.
The prototypes do not substitute a different protein or silently change renderer
or quality. Retaining a stale model is not implemented.

## Evidence

Research checked on 16 November 2026. Source links are pinned where possible.

- [FAH visualisation delivery and command handling](https://github.com/FoldingAtHome/fah-client-bastet/blob/7c4363ae7981ece320047e26200fc244442a91bf/src/fah/client/Remote.cpp)
- [FAH visualisation collection and WU cleanup](https://github.com/FoldingAtHome/fah-client-bastet/blob/7c4363ae7981ece320047e26200fc244442a91bf/src/fah/client/Unit.cpp)
- [FAH viewer styles, sky and grid](https://github.com/FoldingAtHome/fah-web-client-bastet/blob/c54888af2a261d79270a390f289a4972965cedb4/src/Visualization.vue)
- [Apple Screen Saver framework](https://developer.apple.com/documentation/screensaver)
- [Microsoft screen saver integration](https://learn.microsoft.com/en-us/windows/win32/lwef/screen-saver-library)
