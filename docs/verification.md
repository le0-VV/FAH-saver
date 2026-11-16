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

## Windows

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
