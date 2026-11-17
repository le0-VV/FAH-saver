# Prototype verification — 16 November 2026

The native prototypes receive real FAH data and render through the operating
system graphics APIs. The browser candidates provide matching test scenes.
These checks establish feasibility. Sustained performance acceptance is open.

## Data and common checks

- Windows FAH 8.5.6 supplied real protein topology and snapshots.
- Native macOS capture succeeded through separate LAN and Tailscale SSH tunnels.
  The Tailscale check was on the same site, rather than a true off-site test.
- The repeatable captured model has 11,412 atoms, 21,426 bonds and three frames.
  Its SHA256 is `5afc3ed239cb7b6e73f0f705523722c224848fb1b9525e00b1be1b1bcf3d7a68`.
  The anonymised recording stays outside Git.
- Core parser, bounds, mesh winding, camera, all three styles and capture
  round-trip tests passed. AddressSanitizer and UndefinedBehaviourSanitizer
  checks passed for the common core.
- Source tests passed WU replacement, explicit WU selection, malformed data,
  frame gaps, missing visualisation data and reconnect handling.
- JavaScript data/geometry/frame-cap tests and mocked GPU-query lifecycle tests
  passed. Browser GPU timings are explicitly unavailable when unsupported or
  invalidated.

The user chose to keep the upstream 25 MB cache limit for now. Latest mode uses
the latest snapshot FAH exposes. Loop mode uses its available cached snapshots.
No companion collector is included. A later WU transition was observed, but the
earlier WU's capped state was not verified before its working files disappeared.

## macOS

The arm64 Release build and command-line boundary checks passed. The bundle is a
real ScreenSaverView module. It loaded, showed its configuration sheet and
passed ten start/stop cycles in a test host. Brief full-screen module-host tests
passed automatic timeout, app-posted Escape and genuine mouse-input exit.
Persistent preferences stayed unchanged and source activity stopped on exit.

A short warmed full-screen run on Apple M1 used the captured model in
ball-and-stick mode with a sky/grid background:

| Measurement | Result |
| --- | ---: |
| Physical drawable | 2880 × 1800 |
| Warm-up | 1 second |
| Measured interval | About 4 seconds |
| Submitted/completed frames | 240 / 240 |
| Recorded presentations | 239 |
| Presentation cadence | 60.00019 fps |
| p99 presentation interval | 16.66671 ms |
| p99 GPU duration | 7.722 ms |
| GPU errors | 0 |

The same-size preview control recorded 60.00025 fps and 7.776 ms p99 GPU duration.
These short observations do not prove sustained power use or every style at
every resolution. They use the real module in a test host, not ScreenSaverEngine.
The helper initially added an extra drawing timer; that confounded earlier
callback counts and has been removed. Production pacing did not need to change.

All three model styles also passed a fixed-angle culling check: enabling
back-face culling changed zero pixels in each 1280 × 720 reference image.

The installed arm64 bundle was updated to 0.1.2 after a remote-connection fault.
FAH now accepts the authorised Mac through scoped LAN and Tailscale listeners,
access rules and a Windows firewall rule. One graceful client restart preserved
the same WU, which resumed and advanced with its resource settings unchanged.
After closing the temporary SSH tunnel, independent native receivers obtained
two snapshots through each numeric LAN and Tailscale endpoint. The full tailnet
DNS name also supplied a snapshot. System proxy settings stayed unchanged.
The seven CTest checks passed. A session-local explicit `DIRECT` PAC policy was
needed because disabling proxy-enable flags alone did not bypass the system
proxy for remote numeric WebSockets.

The installed bundle's signature and binary hash matched the build. System
Settings retained older preview hosts during replacement. Those hosts were
refreshed, but live system-preview rendering is not confirmed. At the user's
request, UI testing stopped so they can check the installed saver. Temporary
file probes were removed from the installed build; the original LAN source and
other selected options were restored. True off-site operation remains untested.

The user then confirmed that the system saver works, but the other display stays
black. Read-only checks found two online, unmirrored displays, FAH Saver selected
for all displays and no per-display overrides. Version 0.1.3 explicitly enables
layer backing on the root ScreenSaverView before adding the Metal child view.
This is a compositing candidate, not a confirmed resolution of the reported
macOS 26 secondary-display bug [FB19206021](https://developer.apple.com/forums/thread/787444).
[Apple's layer-backing contract](https://developer.apple.com/documentation/appkit/nsview/wantslayer)
applies to a view and its descendants, rather than its ancestors.

The hidden module host passed ten concurrent two-view lifecycle cycles: stopping
one view left the other animating, both stopped cleanly, no source started and
persistent settings stayed unchanged. This check submitted no display drawables
and does not prove that the second physical display renders. Seven CTest checks
also passed. Full-screen verification is left to the user.

## Version 0.1.4 options and particle investigation

Both native versions add a version label in the options panel, light/dark
sky-and-grid presets and a loop-only snapshot interval. The interval defaults to
0.5 seconds and accepts finite values from 0.05 to 60 seconds. Latest mode uses
incoming snapshots without that playback delay. Existing light/solid choices
remain compatible. Zoom was explicitly deferred by the user and is unchanged.

On macOS, nine CTest checks passed, including real offscreen Metal draws at
default/custom interval boundaries, latest mode, invalid timing values and
light/dark/solid pixel checks. The hidden module host checked the actual bundle's
version label, interval control visibility and ten independent two-view lifecycle
cycles. Persistent settings stayed unchanged. These checks do not activate the
system saver or prove display presentation.

Windows portable option/cadence checks passed. The x64 GUI executable rebuilt
with the verified Zig 0.17.1 toolchain, and all four SPIR-V shaders passed
validation. Both preview and saver binaries contain the 0.1.4 version label and
are byte-identical. Windows UI layout, registry round-trip and runtime rendering
of the new preset were not checked on the PC. The installed Mac bundle's hash
and signature matched the tested build. The user retains the system display check.

A bounded current-WU capture contained 4,454 atoms, 4,396 bonds and four snapshots.
It included 121 unbonded atoms, while an earlier 4,163-atom capture had none.
The current bonded framing radius was about 12.18; unbonded particles lay about
29.6–305.7 from its centre. The protein-sized depth planes can clip those particles
during rotation. A fixed-snapshot rotation produced short visibility segments,
but did not reproduce a literal one-display-frame flash. Metal instance uploads
wait for the preceding command; Vulkan uploads wait for the submission fence.
No buffer race or malformed transform was established. Particle visibility,
camera bounds and clipping are unchanged. Anonymised captures reduce unknown
elements to material 999, so they do not identify the particles' chemical species.

FAH's upstream viewer draws every supplied atom. Its dark/light UI themes are
separate from its opaque animated sky shader. The new native presets remain
inexpensive approximations. Sources: [viewer](https://github.com/FoldingAtHome/fah-web-client-bastet/blob/c54888af2a261d79270a390f289a4972965cedb4/src/Visualization.vue),
[sky](https://github.com/FoldingAtHome/fah-web-client-bastet/blob/c54888af2a261d79270a390f289a4972965cedb4/src/viewer/sky.frag)
and [dark theme](https://github.com/FoldingAtHome/fah-web-client-bastet/blob/c54888af2a261d79270a390f289a4972965cedb4/src/dark.styl).

## macOS 0.1.5 options-window lifetime

The user reported that Options sometimes did not open. The ignored click was
observed in System Settings with the installed 0.1.4 hosts. The programmatic
options NSWindow still used the default release-on-close setting, which
[Apple requires ARC clients to disable](https://developer.apple.com/documentation/appkit/nswindow/isreleasedwhenclosed).
Version 0.1.5 sets that flag to false.

The new lifecycle check rejected the unsafe ownership flag before the fix, then
passed ten Save and ten Cancel cycles with sheet attachment, completion, explicit
close and reopen. It checks saved values and discarded edits in a unique test
preference domain. Test windows are transparent and ignore mouse input; no FAH
source starts. All ten CTest checks passed after the fix.

The live System Settings session continued ignoring Options after replacing the
bundle and reloading its old saver hosts. A normal quit and reopen of System
Settings cleared that state. Options then opened three consecutive times and
showed Version 0.1.5 (6). Two were cancelled without edits; the third was left open
for the user. The installed signature and binary hash matched the build. These
observations establish a corrected ownership fault and recovery of the current
host session; they do not prove the intermittent host issue cannot recur.

## Version 0.1.6 automatic latest-model cache

Manual Capture file/Browse controls and the source-type selector are removed
from screensaver Options. Legacy file-only preferences no longer select a
recording. Developer capture/preview tools remain for repeatable checks.

The shared cache stores one complete topology and one latest snapshot, bound to
the configured source URL and optional requested WU through a stable identifier.
The stored record contains no plaintext URL, donor or client state. Writes use
one coalescing worker and atomic replacement; POSIX files use mode 0600. No
cache IO runs on display-frame callbacks. Cache errors are logged and live
reception continues. The first live connection is required when no matching
valid cache exists.

Both hosts load the cached model before starting the live source. The displayed
model is separate from pending topology/history: disconnects, reconnects, errors
and a new WU's topology retain the latest complete snapshot. A matching live
frame replaces it. Loop history stays session-local; an offline stand-in has one
snapshot. Connection messages do not cover a valid model. Renderer and invalid
configuration errors remain visible.

All 13 integrated Mac CTest checks passed, covering atomic persistence, source
isolation, malformed cache rejection, queued-write coalescing, complete-model
handover, late-callback rejection, options lifecycle and GPU rendering. The
updated hidden two-view module host also passed with callback counts unchanged
after stop and persistent settings unchanged.

The Mac's real sandbox cache was primed from the saved live source: 3,226 atoms,
6,075 bonds and one snapshot in 295,925 bytes, after receiving 32 live snapshots.
An isolated host then loaded the actual installed bundle and that real cache
without a network source. Its model was available before source startup and
remained available after an injected offline error; the connection label stayed
hidden in both phases. The installed hash/signature matched the build. System
Settings showed Version 0.1.6 (7) and no capture-file controls; the user's options
were not changed. This is model-readiness and small-options verification, not a
full-screen or sleep/lock test.

The Windows x64 GUI and console test executables cross-built. Native console
runs passed LiveModel and options checks. The first cache run failed with OS
error 5; a stage-only probe located it at concurrent reads and replacement.
Windows publication now uses public FileRenameInfoEx POSIX replacement semantics,
so existing readers keep the old snapshot and new opens receive the replacement.
The corrected native cache test passed in 0.169 seconds, including a deterministic
held-reader check, concurrent replacement and the existing read-only denial
checks. No retries or read-only override were added. Owned PC test directories
were removed after each bounded console run.

The final Windows GUI build includes that fix. It was statically checked and
packaged; full saver UI/rendering of the update was not exercised on Windows.
The Mac cache check passed again after integration. Rebuilding and signing
produced byte-identical Mac bundle data to the already installed build, so the
Windows correction required no Mac replacement.

## Version 0.1.7 interval editing and retained-view investigation

The interval default changes to one second without overwriting explicit saved
values. The artificial 60-second maximum is removed; values must remain finite
and at least 0.05 seconds. Native edit controls reject characters other than
ASCII digits and one decimal point, including invalid pasted replacements.
Empty text and a lone dot are allowed while editing. Save validation uses a
small inline message instead of a modal alert. Large saved values reopen as
plain decimal text rather than exponent notation.

Focused Mac decimal, CLI, options lifecycle and GPU checks passed. Save/reopen
coverage includes 600 seconds, 1e100 and the largest finite double, together with
invalid input preserving preferences and leaving the options sheet open.
The Windows decimal and options console tests also passed natively with empty
stderr, in about 0.045 and 0.040 seconds respectively. Their owned payload and
temporary directory were removed. Windows native Options interaction was not
exercised by those console checks.

The user reported a 1+ GB legacyScreenSaver process. Before any restart, PID
77165 had a 1.1 GB physical footprint. Its memory map showed about 641 MB of
resident IOSurfaces, including 30 2880x1800 Metal colour drawables and four HD
colour drawables. Allocated malloc memory was 49.2 MB. Heap inspection found nine
saver views, renderers, Metal views, layers and source objects. This establishes
retained presentation resources as the main observed cost; it does not identify
all allocations or prove every view was receiving stop notifications.

A hidden probe against the installed 0.1.6 bundle reproduced a lifecycle gap:
hiding or removing a retained view left its timer/source active; an explicit
stop finally stopped both. No user PID, live source or visible window was used
for that probe. Raw memory maps remain outside Git.

The saver now suspends on actual hide/ancestor-hide and removal from an existing
superview. It stops timer/source/cache work and releases the MTKView and its
CAMetalLayer owner, rather than retaining their drawable pools in an inactive
view. Showing or reattaching resumes only prior or explicitly deferred start
intent; an explicit stop cancels it. Initial windowless hosting still works.
Twenty retained-view lifecycle cycles passed with old Metal view/layer weak
references becoming nil and no further inactive callbacks or submissions.

All 15 integrated checks passed before deployment. The old host was then
replaced after preserving its diagnostics. In the new renderer PID 2262, five
Settings close/reopen cycles resulted in two saver/renderer/Metal/layer/source
objects at 223.9 MB. Five more cycles in the same PID retained the same counts,
the same 57.1 MB IOSurface total and a 222.8 MB footprint. This is a bounded
same-process stability check; it does not establish long-session or every
screensaver activation path.

Actual options interaction exposed a gap in the initial formatter-only tests:
the field editor could still accept a letter. The correction uses the public
native field-editor path to reject proposed edits before mutation. Regressions
now exercise real NSTextView insertText, keyDown and named-pasteboard handling,
not just the formatter validation method. Invalid Save feedback is placed
directly below its field, with lower rows shifted to avoid overlap.

The final 0.1.7 (9) installed bundle was checked in System Settings. After an
invalid zero value focused the interval editor, typing `a` left the value at
zero and showed decimal-only feedback directly below that field. A value above
the former limit remained editable. Test edits were cancelled; reopening Options
restored the user's saved one-second interval and other settings. The final
integrated 15-check suite passed again, including actual native editor key/paste
handling and field-error geometry. The installed hash/signature matched the
final build. Windows GUI artifacts cross-built with field-local errors; native
Windows GUI interaction remains untested.

## Version 0.1.8 latest-mode startup and further graphics cleanup

FAH 8.5.6 publishes `units[index].frames = N` before sending coordinate index
`N - 1`. A cold `viz` request still starts at zero to obtain topology; the API has
no cold latest-only flag or replay-end packet. SourceOptions now has an opt-in
latestOnly flag. SourceSession validates historical packets but withholds Frame
callbacks below the selected WU's currently published tail. Both native latest
hosts and live previews opt in; loop/capture callers retain full delivery. The
cache remains visible until an eligible complete live frame arrives. There is
no timer-based catch-up guess. Removed count metadata becomes unknown; coordinates
without a positive watermark fail explicitly rather than falling back to replay.
Sources: [FAH Remote](https://github.com/FoldingAtHome/fah-client-bastet/blob/93b96817a82ade9769fba3571ac697788f13357a/src/fah/client/Remote.cpp),
[Unit](https://github.com/FoldingAtHome/fah-client-bastet/blob/93b96817a82ade9769fba3571ac697788f13357a/src/fah/client/Unit.cpp).

All 15 integrated checks passed. A bounded live latest-only probe delivered one
callback starting at index 96, rather than replaying indices from zero. An earlier
three-second latest probe ended before any eligible frame, so catch-up can take
longer than that short probe's deadline. A subsequent comparison encountered a
new WU with one frame, for which both modes correctly delivered index zero.
Native Windows synthetic protocol checks passed with exit zero and empty stderr.

The user's new memory report was confirmed in PID 22934: 666.5 MB footprint,
900.3 MB peak, four saver/renderer/Metal/layer/source objects and only 10.9 MB of
allocated malloc memory. The report was therefore not a single model-history
allocation. After the user's Settings-preview start/stop, the same four objects
remained at 677.8 MB. This did not establish normal idle or lock/sleep behaviour.
Only a start notification was captured; shutdown notification delivery was not
established by that observation.

A retained-window probe found that closing its window could leave a view running
without a hide/detach callback. The saver now observes its actual window's public
will-close notification and stops that view. Apple-family GPUs also use memoryless
D32 depth for the existing clear/discard single-pass attachment; other supported
GPUs keep private D32 depth. Full resolution, depth precision and colour buffer
count are unchanged. All three styles produced byte-identical 960x600 reference
PNGs versus private-depth rendering, with no GPU errors.

The installed 0.1.8 (10) hash/signature matched the tested build. A fresh active
host had one renderer and a 163.6 MB footprint, with 2.8 MB of owned graphics
allocations. A lower fresh-process reading does not prove that every retained
host path or longer-session growth is fixed. Options showed the correct version
and Follow Latest, without changing saved settings. The user retains full-screen
and longer-session visual acceptance.

## Version 0.1.9 inactive legacy-host rendering

On 17 November, the user reproduced GPU use after leaving a Settings full-screen
preview. The installed 0.1.8 host used 12.1–13% CPU while inactive, with three
saver/renderer/Metal-view/layer/source instances and a 561.7 MB footprint.
Protected thread-state access prevented usable sample stacks. An external
distributed screensaver-notification observer received no start/stop events
during this preview.

A temporary, bounded diagnostic build then counted actual Metal command commits
and completions inside the host. All three non-preview views continued at about
60 submissions per second for over a minute after exit. Their geometry, hidden
flags and window visibility stayed unchanged. The public occlusion visible bit
was unset both during and after the visible preview, so it was unsuitable as a
visibility gate. No stop, hide, detach, close or relevant occlusion event arrived.

Workspace activation did change from Login Window on entry to another app on
exit. A first view started about two seconds before Login Window became active,
and a separate frontmost-app query briefly lagged the exit event. The production
guard therefore applies only to non-preview views in the legacy host, permits an
initial cold/windowless start, and uses the activation notification's application
identity. After observing the active session, it performs a normal explicit stop
on exit. The dismissal latch blocks stale inactive starts, including after an
unrequested later session. Only an actual start clears the latch; explicit stop
cancels pending restarts. The production build has no diagnostic sampler or
per-frame diagnostic counters.

The public API contract is documented by Apple:
[workspace activation notification](https://developer.apple.com/documentation/appkit/nsworkspace/didactivateapplicationnotification?language=objc),
[frontmost application](https://developer.apple.com/documentation/appkit/nsworkspace/frontmostapplication?language=objc).
Login Window ownership is a compatibility rule verified on this Mac's legacy host,
rather than a documented guarantee for every screensaver host.

All 16 integrated checks passed, including the new session fixture, existing
presentation/cache/options checks and offscreen Metal rendering. The session
fixture checks cold/windowless start, duplicate notifications, stale start
requests, an unrequested later login cycle, explicit-stop precedence, independent
display requests and genuine preview isolation.

The user then ran and exited two Settings full-screen previews with the corrected
guard and temporary counters. Both cycles used the same host process. Six
non-preview view instances were observed across the cycles; two were later
deallocated and all four remaining views stopped. Their final submission counts
stayed fixed for more than 79 seconds after the second exit. Each retained view
reported no active source, cache worker or Metal presentation view. A separate
heap inventory also found no MTKView, CAMetalLayer or MacSource instances.
Repeated post-exit CPU readings were 0.0%, with unchanged CPU time. Physical
footprint was 63.2 MB after exit versus a 650.9 MB peak in that same process.
These readings were captured before any host reload.

The normal 0.1.9 (12) module then replaced the diagnostic candidate. Its installed
binary hash is `57ef6f0ae3657cfd0fdb8363e59d155a85a5e50d5d2954d13125e9e46d84fc12`,
matching the tested build; the ad hoc signature was verified. Temporary
instrumentation was absent from the normal binary, and the inactive diagnostic
host was reloaded only after the measurements. Real-host validation covered
Settings full-screen previews on this Mac. Normal idle activation, lock/sleep and
long-session behaviour remain unverified. This release changes only the Mac
host; the existing Windows package remains at 0.1.8.

## Memory profile on 17 November 2026

This profile used the normal installed 0.1.9 (12) build on the M1 Mac. The user
ran a Settings full-screen preview while native `footprint`, full `vmmap` and
`heap --noContent` snapshots were captured. Measurements below use MiB
(1,048,576 bytes). The new host peaked at 471.02 MiB; active snapshots ranged
from 328.92 to 456.42 MiB. The earlier 650.9 MiB peak had no simultaneous full
category snapshot. The following table accounts for the fresh 456.42 MiB
snapshot, rather than assigning invented categories to that older peak.

| Charged category | MiB | Evidence |
| --- | ---: | --- |
| Display IOSurfaces | 150.312 | Six Retina and three HD CAMetalLayer drawables, plus one compositor surface |
| Owned physical footprint, unmapped | 250.125 | Kernel-owned charge; exact internal resource names unavailable |
| Tagged graphics allocations | 20.203 | IOAccelerator and separately tagged unmapped graphics memory |
| Core Animation and Core Graphics | 18.938 | Window/composition allocations |
| Heap and allocator metadata | 11.734 | All ordinary allocated objects/arrays and allocator pages |
| Other runtime, stacks and page tables | 5.112 | Remaining charged categories |
| **Total** | **456.424** | Sum of native footprint categories |

These categories sum exactly in bytes before rounding. Resident size, virtual
address reservations and `MTLDevice.currentAllocatedSize` are separate views of
allocations and must not be added to this total. The separately reported
compressed/swapped values are already included in the footprint tool's charged
category totals. The interpretation follows Apple's
[Metal memory analysis guide](https://developer.apple.com/documentation/xcode/analyzing-the-memory-usage-of-your-metal-app).

The full VM map identified the display surfaces precisely: six 2880x1800 Metal
drawables at 20.125 MiB each, three 1920x1080 drawables at 8.094 MiB each, and a
5.281 MiB Core Animation surface. This is two Retina-sized renderer pools plus
one HD-sized pool for the two physical displays. The user-confirmed post-exit
snapshot still contained one MTKView/CAMetalLayer/MacSource, used 153.64 MiB,
and later consumed 4.4% CPU. Settings was on General. The administrator ownership
scan measured 152.31 MiB and still left 47.50 MiB in the unnamed unmapped
category. It did not reveal resource-level names. This is a remaining inactive
view path; the earlier two-cycle success did not establish all host paths.
Its preview/session state and creation sequence were not captured in this run,
so neither an unguarded preview nor an unarmed late-created view is established
as the cause.

The cached model used for independent accounting had 5,894 atoms, 10,929 bonds
and one snapshot. Topology arrays requested 181,736 bytes and coordinates
70,728 bytes. Active latest-mode source/display data plus a space-filling CPU
scene requested 976,688 bytes per view, or 2.79 MiB for three views. Bond styles
requested 1,851,008 bytes per view. These are part of the heap total, not extra
memory to add. Cache serialization peaked at 5,902,704 requested C++ bytes for
this fixture and released its temporary allocations. Latest mode did not retain
a trajectory history. Explicit instance buffers and static meshes were small;
the M1 depth attachment was memoryless and reported no allocated backing bytes.

A separate native arm64 probe linked the unchanged renderer and used three
IOSurface-backed colour targets per renderer. It drained an autorelease pool
for each draw and bounded concurrent commands to three per renderer. The
offscreen pools approximate CAMetalLayer storage; the actual host surface
sizes above remain authoritative.

| Controlled case | One pending command per renderer | Up to three pending commands | Explicit Metal resources |
| --- | ---: | ---: | ---: |
| One Retina renderer, all styles | 125.47 MiB | 168.50 MiB | 61.34 MiB |
| Two Retina plus one HD, all styles | 301.60 MiB | 425.97 MiB | 147.05 MiB |
| Two Retina plus one HD, space-filling only | 285.77 MiB | 431.17 MiB | 144.52 MiB |
| Four Retina renderers, all styles | 459.30 MiB | 643.02 MiB | 243.27 MiB |

Changing concurrency left explicit Metal resource sizes unchanged. It added
124.38 MiB in the three-renderer all-style comparison and 145.41 MiB in the
space-filling comparison, with only about 1 MiB of additional malloc use. The
background-only three-renderer comparison added 34.80 MiB. This links a large
part of the changing footprint to command/draw-dependent graphics working
memory beyond explicit model buffers. It does not identify every internal
kernel allocation in the live host's unnamed category.

Release timing matters. In the three-renderer concurrent case, releasing colour
targets/model data and waiting two seconds reduced footprint from 425.97 to
27.96 MiB while queues and pipelines remained alive. Releasing queues and
waiting another second reduced it to 19.97 MiB. Most release occurred before
queue destruction. Earlier scratch results with a batch-wide autorelease pool
were excluded because they retained command buffers differently from production.

Per-allocation tracing of the protected system host failed under System Integrity
Protection. Protections were retained. Failed/incomplete Instruments traces were
excluded. Raw VM/heap/footprint reports, the native probe and allocation-accounting
helpers remain private under `/private/tmp`; only aggregate results are recorded
here. These findings prioritise the remaining inactive/extra renderer and GPU
frame concurrency. Changing drawable buffering needs a 60 fps presentation test.
Profiling made no production code or installation changes.

## macOS WebView replacement experiment on 17 November 2026

The user requested a real WebView engine replacement and RAM/GPU comparison,
then scoped it to macOS first. The experiment retained native FAH networking,
latest-frame replay suppression, the persistent model cache and native Options.
Its bundled local WebGL engine shared geometry/shaders with the comparison page;
no remote page or library was loaded. Backend and clock selection were explicit,
with no runtime fallback or quality change.

The initial WKWebView candidate was blank on both displays. Synchronous first
model drawing fixed the initial-frame dependency, but a visible standalone rAF
trial still had zero draw growth over ten seconds. Its low resource readings
were excluded. A coalesced native clock produced continuous drawing at about
60 draws/s in the standalone app. In the actual legacy host, the model appeared
and animated briefly, then both displays went blank while draw requests
continued. Event logs showed three ready contexts, correct 2880x1800/1920x1080
backing sizes and no JavaScript error before user exit. This resembles the
[WKWebView legacy-host display issue discussed with Apple DTS](https://developer.apple.com/forums/thread/820860),
but that source does not establish the precise cause on this Mac.

A separate public in-process WebView backend with the native clock stayed visible
and animated on both displays in the user's approximately 20-second check.
This was the working candidate 0.2.0 (15d1) used for the final comparison. It uses
the older WebView API; Apple recommends WKWebView for modern applications in its
[WebView documentation](https://developer.apple.com/documentation/webkit/webview-swift.class).
The legacy candidate remains an experiment, not a newly selected production API.

Before timing, hidden fixed-time PNG checks compared Metal, WK and Legacy with
the same real one-snapshot fixture at 960x600 in all three styles. Dimensions
were exact. Metal/Web pixel equality was 99.9943% for space-filling, 99.9825% for
ball-and-stick and 99.9724% for sticks. At most three pixels per image differed
by more than two channel levels; WK and Legacy output matched each other.
The harness was corrected to preserve requested content size rather than count
titlebar area as a viewport. A failed Legacy 960x536 reference was excluded.

### Matched full-resolution trials

Hardware was this Apple M1 Mac on macOS 26.7.2. Every valid trial used the same
5,894-atom/10,929-bond single frame, capture SHA256
`6e2dbc86da4cc14623899a429f7c4335985f618ef3ea5476e9eba5b8c3a9ed99`,
space-filling style, identical solid colour/camera/geometry, no MSAA and a 60 fps
cap. Layouts were one 2880x1800 canvas, or that canvas plus one 1920x1080 canvas
on the other display. Actual canvas dimensions were checked at runtime.

Two trials per engine/layout ran in opposing order: Metal, WebView, WebView,
Metal. Each used 10 seconds of warm-up, 30 seconds of measurement and 10 seconds
of stopped cooldown while the app remained alive. Expensive renderer timing
was disabled for normal-cost trials. Successful submission/draw rates were
59.78–59.99 per second; these are not verified display-presentation timestamps.

| Layout | Metal steady footprint | WebView steady footprint | Metal CPU | WebView CPU |
| --- | ---: | ---: | ---: | ---: |
| One Retina canvas | 131.7–133.6 MiB | 212.2–246.5 MiB | 4.62–5.09% | 8.88–11.92% |
| Retina plus HD | 183.6–205.0 MiB | 337.6–349.4 MiB | 8.08–8.43% | 21.42–24.51% |

Memory ranges are the two trials' median simultaneous process footprints, not
independent process peaks. CPU percentages refer to one logical core. The
external sampler used process responsibility to include associated WebKit
services rather than measure only the host. The working in-process candidate
and Metal each had one observed process during steady measurement. A separate
WK/native-clock accounting check had four; it was not substituted into the
Legacy result. Sampling was approximately 0.25 seconds, with no process-read
errors in the eight full-resolution trials. Native CPU counter units were
validated as Mach ticks against `getrusage` before conversion.

The mean of the two steady footprint medians was 132.7 versus 229.4 MiB for one
canvas, and 194.3 versus 343.5 MiB for both displays. WebView therefore used
about 73% and 77% more memory respectively in this test. Mean CPU use was
4.86 versus 10.40% for one canvas and 8.26 versus 22.96% for both.

### GPU evidence and limits

The WebGL elapsed-time extension was unavailable. Whole-device IORegistry
utilisation was sampled, but one single-canvas Metal repeat retained high GPU
activity after the app exited. That counter was not treated as isolated app GPU
cost. The user then started a bounded, read-only administrator `powermetrics`
collector for the dual-display comparisons. This M1 did not expose per-task GPU
time despite the requested flag; only hardware GPU busy-time, frequency states
and energy estimates were available. System protections were unchanged.

After trimming phase boundaries, each dual-display trial had 26 complete native
hardware samples. Whole-GPU active residency was 60.64–60.98% for Metal and
65.46–68.36% for WebView. Estimated energy rate also showed no WebView saving.
These are whole-device measurements on the same Mac, not per-process GPU
durations or a universal energy benchmark. Other activity and DVFS can affect
them. Together with the consistent RAM/CPU regression, they provide no evidence
that this WebView replacement improves the requested resource use.

The user chose **Restore Metal** after reviewing the measured result. The exact
installed native 0.1.9 (12) bundle was restored, with SHA256
`57ef6f0ae3657cfd0fdb8363e59d155a85a5e50d5d2954d13125e9e46d84fc12`
and verified ad hoc signature. Production source/build selection was also
returned to Metal. The complete Web module experiment is preserved on
`agent/webview-saver-experiment`; independent Web libraries, runtime tests,
reference generation and the comparison app remain available on the normal
branch. Existing native inactive-view limitations remain separate work.

Raw model fixtures, images, process samples, native hardware records and
diagnostic logs remain private under `/private/tmp/fah-render-compare-20261117`
and the named host-check files. Only aggregate results are recorded here.
All trials and the administrator collector were stopped before completion.
After the source selection was restored, all 22 integrated CTest checks passed
in 34.58 seconds. The shared JS suite and native process-accounting self-check
also passed. The installed native version/build/hash were checked again after
restoration; its exact 0.1.9 release bundle was retained.

## Windows prototype measurements

The complete x64 Vulkan host and native WinHTTP source cross-built with a
checksum-verified temporary Zig toolchain. Options and projection/winding tests
also ran successfully on the PC. Capture playback, a brief full-screen input
exit and live loopback rendering exited normally. Window screenshots were
withheld because of overlap; visual image parity remains unverified.

The asynchronous WinHTTP cancellation probe passed 35 cycles across immediate
stop, a stalled HTTP upgrade, an idle WebSocket and frame delivery. Maximum
measured stop time was 1.0185 ms, with zero callbacks after stop.

A first matched Windows comparison used 1280 × 720 client pixels, the same
capture, ball-and-stick, loop playback, sky/grid and a 60 fps cap:

| Measurement | Vulkan | WebView2/WebGL2 |
| --- | ---: | ---: |
| Reported count | 299 submissions | 301 draw callbacks |
| Reported interval | 5.2348 seconds | 5 seconds |
| Mean GPU-query duration | 0.7437 ms | 0.6480 ms |
| Completed GPU-query samples | 298 | 300 |

Submission counts and browser callbacks are not display-presentation timestamps.
The original native elapsed interval also included renderer teardown, so its
57.12 submissions/s must not be treated as a steady-state frame-rate result.
The browser reported 60 callbacks/s. Both candidates used the Radeon RX 9060 XT;
WebView2 used ANGLE/D3D11 and runtime 157.0.4341.46.

The short native run reported unexpectedly high CPU use. An initial diagnostic follow-up
measured 3.938 process CPU seconds over 5.246 wall seconds, with only 0.109 CPU
seconds on the GUI/main thread. It recorded 304 pump iterations, 299 timer wakes
and four input-queue wakes. Those counts give no evidence of a large application
message-loop spin. A run without metrics still used 4.109 process CPU seconds
over a 5.434-second whole-process lifetime; its GUI thread used 0.219 CPU seconds.
The remaining CPU belongs to other process threads. Their identity and stacks
were not identified by those counters.

The corrected interval report then measured 4.266 process CPU seconds during
5.013 active seconds, with only 0.016 CPU seconds during 0.224 seconds of source
and device shutdown. Successful submission-call cadence was 60.06 calls/s.
This confirms that the old lifecycle rate understated active submission cadence;
it still does not measure display presentation.

A fifteen-second normal run, with metrics disabled, showed the CPU cost settling.
Between samples at 6.008 and 14.002 wall seconds, process CPU increased by about
0.188 seconds and GUI/main-thread CPU by about 0.156 seconds. That is about 2.4%
of one logical CPU for the process over that later interval. Total CPU was 4.453
seconds for the fifteen-second run, close to the five-second normal run's 4.422
seconds. This supports a several-second startup-period burst rather than a
persistent one-core rendering load.

A separate process-specific PSS attribution run matched nine thread lifetimes
across about 1.504 seconds. Its hot worker used 0.796875 kernel and 0.5625 user CPU
seconds, with a thread entry point in `amdvlk64.dll`. All 24 instruction-location
samples mapped to `ntdll.dll`. This identifies the hot startup thread's driver
entry point, not the exact operation or call stack. It does not prove shader
compilation or justify changing the application's frame timer. Context capture
can perturb the target, so this run is attribution evidence, not a benchmark.

The browser's observed process CPU includes startup and can omit unobserved
processes; it is not directly comparable to native post-initialisation CPU.
Browser working-set sums can double-count shared pages. Power and complete
browser process cost are unmeasured. No stack winner is established by this pair.

The first longer browser CPU-series attempt failed before measurement with a
frame-index error. A queued animation timestamp could precede the new start
epoch slightly; loop mode then selected frame -1. A regression test reproduced
that boundary. Playback, camera and style time now start at zero, and the test
passes. The failed attempt is excluded from timing comparisons.

The corrected fifteen-second browser CPU-series retry passed at 1280 × 720,
ball-and-stick, loop playback, sky/grid and a 60 fps cap. Between host-time samples
6.0686499 and 14.0696594 seconds, observed browser CPU increased by 1.34375 seconds;
the reported host CPU counter did not increase at its available resolution.
The five retained browser processes and inventory coverage stayed stable, with
no reported access/sampling errors and no dropped series entries.

| Later-interval CPU | Native normal path | WebView2 host + observed group |
| --- | ---: | ---: |
| CPU seconds per wall second | 0.02346 | 0.16795 |
| Equivalent share of one logical CPU | 2.35% | 16.79% |

The observed browser rate is about seven times the native rate in these small
sequential trials. The browser had GPU queries and process sampling enabled;
the native normal run had metrics disabled. Browser inventory excludes crashpad
and can miss short-lived processes, so this is indicative rather than a complete
system-cost comparison. FAH continued running throughout the tests.

The browser produced 902 draw callbacks in 15.0148 seconds, or 60.0075 callbacks/s.
Mean GPU-query duration was 0.7589 ms, with 901 completed queries and one final
pending query excluded. This is similar in scale to the earlier native GPU
sample, but the trials do not establish a general GPU-time winner. Display
presentation, power and GPU memory remain unmeasured on the Windows comparison.

These observations support keeping the requested native stack as the initial
implementation. They do not replace sustained acceptance tests. The corrected
browser trial removed its temporary profile, and its cleanup audit found no
remaining own processes or temporary tasks.

The normal native path now disables benchmark timestamp queries and counters.
Opt-in aggregate diagnostics record application-thread CPU and wait/record/present
phases without per-frame logging. They do not identify the other worker threads.

## Remaining acceptance work

Use the [comparison protocol](performance-plan.md) for sustained, repeated
measurements. Full system screensaver installation,
lock/sleep recovery, mixed-display behaviour, signed release distribution,
off-site operation and paired FAH-throughput impact remain unverified.

PC access and testing were paused while the user gamed. The completed cleanup
audit found no test processes or temporary tasks remaining, and the temporary
LAN and Tailscale SSH tunnels were closed. The user then explicitly released the
PC with “done gaming”; bounded development checks have resumed.


## Unmapped memory attribution follow-up, 17 November 2026

The dominant unexplained charge is now identified with high confidence as internal
GPU-driver geometry working memory. Apple's allocation inspector names the large
resources **Parameter Page List Backing**. This is stronger evidence than the
previous inference from command concurrency alone. It is not an object-by-object
byte reconciliation of the historical 650.9 MiB peak.

### Allocation names and real-host evidence

The Apple-signed command below exposes GPU allocation descriptions that the generic
`footprint` category does not show. Run it outside the sandbox, explicitly native:

```sh
/usr/bin/arch -arm64 /usr/bin/IOAccelMemory --pid PID --one-line
```

This read-only query took roughly 30–50 seconds because it also waited for
annotations from unrelated graphics clients. No purge operation was used.
Do not sum all rows as a physical-footprint total: texture wrappers and IOSurface
backing can describe the same storage. Its rounded allocation sizes also cannot
be directly subtracted from a separate process-charge snapshot.

The real host had four `FAHSaverView`/`FAHRenderer`/`MTKView`/`CAMetalLayer`
instances. A captured process footprint was 1,384.08 MiB, including 1,054.25 MiB
of generic unmapped memory. All 8,434 dirty objects in that category were wired
128 KiB objects. The GPU inspector listed eight large Parameter Page List Backing
resources, approximately 150–174 MiB each. These are asynchronous samples, not an
atomic allocation ledger. The approximate 1,344.3 MiB sum of those named resource
sizes must not be treated as an unexplained residual against the 1,054.25 MiB
process charge.

The cached work unit had changed since the earlier benchmark. The current model
contained 17,933 atoms, 17,736 bonds and one frame; its JSON payload was 1,491,349
bytes. The old comparison fixture contained 5,894 atoms. The sphere mesh has 352
triangles per atom, so the current space-filling model submits 6,312,416 atom
triangles per frame per renderer. Small shared input meshes therefore do not
imply small transformed-geometry working memory.

A native, all-processes Metal System Trace succeeded without attaching to the
protected host. Its PID-filtered events corroborate the driver allocation path:

- 19,911 driver memory events included 16,243 successful 128 KiB wire requests
  and 2,748 128 KiB unwire requests. Requests can concern the same memory more
  than once; these cumulative counts are not unique allocated objects.
- The 37 explicit Metal resource events covered static mesh buffers, model
  buffers, display drawables and Core Animation resources. Three model buffers
  were exactly 1,434,640 bytes each: 17,933 instances times 80 bytes. Their
  backtraces reached `FAHRenderer updateSceneAtTime:`, `renderToTexture:`,
  `drawInMTKView:` and `FAHSaverView animateOneFrame`.
- The large Parameter Page List Backing resources were not explicit application
  Metal resource events. The derived kernel-resource-allocation table had no
  rows. Driver wire events did not expose an allocation ID that could join each
  charged VM object to an individual named backing resource.

Apple's installed `GPUCounterGraph.plist` maps
`ParameterBufferPrimitiveBlockAlloc` to Tiled Vertex Buffer primitive-block bytes,
including indexed primitives, transformed vertex data and vertex-format data.
`ParameterBufferBytesUsed` also includes per-tile primitive visibility. Apple's
[GPU architecture explanation](https://developer.apple.com/videos/play/wwdc2020/10602/)
describes this geometry intermediate storage between tiling and rendering.
Together, the resource names, trace events and controls identify the main
allocation class. The exact charge for each named backing object remains
unresolved; the samples use different times and accounting views.

### Geometry and concurrency controls

These windowless probes used the production renderer, one 2880x1800 renderer,
three IOSurface-backed colour targets, per-draw autorelease pools and bounded
pending commands. The first four rows retained the same complete 5,894-atom
capture, CPU scene and instance buffers, changing only the atom draw count.
Every generic dirty object was again a wired 128 KiB object.

| Atoms drawn, one pending command | Generic unmapped charge |
| --- | ---: |
| 0 | 5.00 MiB |
| 1,000 | 12.00 MiB |
| 3,000 | 25.25 MiB |
| 5,894 | 43.625 MiB |

With the current 17,933-atom fixture, one pending command produced 113.125 MiB
of generic unmapped charge and a 195.00 MiB process footprint. Up to three pending
commands produced 222.125 MiB and a 310.11 MiB footprint. The explicit model and
colour allocations stayed unchanged. After resource release and two seconds,
the generic dirty charge was zero and the process footprint was 20.41 MiB.
These are controlled allocation measurements, not display-presentation tests.

A held but idle renderer was insufficient for the slow GPU inspector: its generic
charge could fall from 43.625 MiB to zero during the query. A separate continuous-
render control retained the named parameter backing and the generic charge.
This prevents interpreting idle cleanup as evidence that the named resource was
never associated with the rendering workload.

### Profiling and shutdown limits

The command runner for this session was translated (`sysctl.proc_translated=1`).
An earlier Instruments attempt therefore reported that kernel tracing could not
run under Rosetta. Profiling commands were changed to explicit ARM64. The earlier
high-memory screensaver host and the new host both reported `translated=false`;
the installed saver bundle is arm64. Native own-process Game Memory attachment
still failed and was excluded. Native all-processes Metal System Trace completed.
System Integrity Protection remained enabled.

The automatic watcher initially used resident size and missed the full charge;
the real-host reports above were captured directly by PID. Resident size excludes
important parts of this unmapped process charge and must not select the profiling
target by itself.

After the user's confirmed preview exit, the host still used CPU. Closing the
preview panel and returning Settings to General did not fully stop it. The host
was terminated after saving its reports, restarted while Settings remained open,
and was terminated again after Settings quit. A subsequent process check found
no running legacyScreenSaver host or diagnostic renderer. This ends the observed
background workload; it does not fix the remaining host lifecycle bug.

Production code and the installed saver were not changed by this investigation.
The actionable optimisation targets are the sphere geometry, bounded pending GPU
work, and extra/inactive renderer instances. Raw captures and scratch probes stay
private under `/private/tmp/fah-unmapped-20261117`; only aggregate evidence is
recorded here. The exact-byte private-framework helper could not query the kernel
because it lacked Apple's private allocation-inspection entitlement; it was
excluded from the results.
