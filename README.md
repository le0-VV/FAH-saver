# FAH Saver

A protein-model screensaver for macOS and Windows. The screensaver can display
the active Folding@home work unit (WU) from the same computer or another computer.

Status: requirements and performance investigation. There is no runnable
screensaver yet. The rendering stack must be selected from measurements.

## Agreed requirements

| Area | Requirement |
| --- | --- |
| Initial platforms | macOS and Windows. Keep a future Linux version possible. |
| Connections | Local host, LAN and remote access through Tailscale. |
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

The macOS candidate remains direct Metal. A Vulkan-on-Metal layer is not an
agreed dependency. Native rendering remains a preference, not a measured result.

## Visual reference and provisional defaults

The current FAH viewer creates a sky and grid scene. It does not use a plain
black scene as its only background. Use that scene as the visual reference.
Proposed initial background options are the FAH-style sky and grid, and a solid
colour chosen by the user. These options and their default remain provisional.

Slow camera rotation is a provisional default. Snapshot playback and style
selection are independent settings. The cycle interval and random-selection
behaviour remain to be specified.

## Verified upstream behaviour

The following findings describe upstream source. They have not been checked
against the FAH client installed on the user's PC.

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

The ordinary v8 data flow appears to need no helper on the folding host. Verify
installed-client compatibility and network access before confirming this.

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

Connection loss, missing visualisation data and a changed WU must be explicit
states. Whether to retain and display a stale model after disconnection remains
an open product choice. Never silently substitute a different protein.

## Evidence

Research checked on 16 November 2026. Source links are pinned where possible.

- [FAH visualisation delivery and command handling](https://github.com/FoldingAtHome/fah-client-bastet/blob/7c4363ae7981ece320047e26200fc244442a91bf/src/fah/client/Remote.cpp)
- [FAH visualisation collection and WU cleanup](https://github.com/FoldingAtHome/fah-client-bastet/blob/7c4363ae7981ece320047e26200fc244442a91bf/src/fah/client/Unit.cpp)
- [FAH viewer styles, sky and grid](https://github.com/FoldingAtHome/fah-web-client-bastet/blob/c54888af2a261d79270a390f289a4972965cedb4/src/Visualization.vue)
- [Apple Screen Saver framework](https://developer.apple.com/documentation/screensaver)
- [Microsoft screen saver integration](https://learn.microsoft.com/en-us/windows/win32/lwef/screen-saver-library)
