# Rendering and networking comparison

Status: proposed test plan. No performance measurements have been collected.
The agreed shipping target is 60 fps with a user-configurable frame-rate limit.
The acceptance thresholds below are proposals, not additional user requirements.

## Candidates and controls

Compare Metal against embedded WebGL on macOS, and Vulkan against embedded
WebGL on Windows. Run each candidate in the real `.saver` or `.scr` host.
Use release builds without validation layers, debug overlays or an open profiler
UI during timed runs. Collect detailed traces separately to explain failures.

Keep the following identical within each comparison:

- Captured topology, snapshot sequence, playback timing and camera path.
- Atom sizes and colours, bond geometry, lighting, anti-aliasing and transparency.
- Drawable pixel dimensions, display scaling, monitor refresh rate and frame cap.
- Background: compare matching FAH-style sky/grid and solid-colour scenes.
- Power source, brightness, GPU selection, driver version and thermal conditions.

Check reference images before timing. Record any unavoidable visual differences.
A renderer that draws fewer atoms or uses a lower resolution is not equivalent.
Use stable fixture files with checksums and atom, bond, frame and byte counts.
Keep account details, machine addresses and credentials out of fixtures and Git.

## Separate the work

1. **Rendering:** replay recorded data locally with networking disabled. Test a
   small model, a typical model and the largest usable captured model. Label any
   synthetic stress data separately from real WUs.
2. **Networking and decoding:** receive the same captured stream without drawing.
   Compare native reception/decoding against the browser path. Include a cold
   connection with history, latest-mode retention, resumed reception and loop
   retention. Measure wire bytes, peak memory, CPU time and time to usable data.
3. **Integration:** repeat representative and worst cases against a live FAH
   source over LAN and Tailscale. Measure both the viewer and source overhead.
   Record whether Tailscale used a direct or relayed path. An actual off-site
   test is required before claiming remote operation is verified.

Display fps measures camera/scene presentation. Snapshot cadence measures new
protein coordinates. Report both. Do not fabricate coordinate updates to claim
60 fps, or confuse a quiet latest-mode feed with a lower rendering workload.

## Trials and measurements

Start with a 30-second screening run for each style, both playback modes and both
backgrounds at 60 fps. Repeat screening for one display and the intended multiple
display layout. Record physical drawable sizes rather than logical window sizes.
Use these results to select representative and worst cases for longer trials.

For each selected case, warm up for 30 seconds, then measure for 120 seconds.
Run three trials per candidate, alternating candidate order. Let temperatures
return to the same stable range between trials. Also screen 30 fps and one lower
supported limit to check that the user setting reduces work and controls pacing.
Record startup and first-frame timing separately, including cold and warm starts.

| Measurement | Required reporting |
| --- | --- |
| Presentation | Delivered fps; median, p95 and p99 presentation intervals; missed target presentations. |
| CPU | CPU time and utilisation, with the core-count convention stated. |
| GPU | GPU time per frame and utilisation where available; active adapter and engine. |
| Memory | Steady and peak resident memory, plus GPU memory where available. |
| Power | Baseline-adjusted power/energy over a stated interval, with the measurement method. |
| Data | Wire bytes, decoder cost, retained frames and peak memory during history reception. |
| Startup | Time to host ready, first usable model and first presented frame. |

Account for the whole candidate: host, helper, browser renderer and GPU processes.
Avoid double-counting shared memory. Identify shared browser processes and isolate
unrelated browser activity before attributing their cost to the screensaver.
Measure three baselines: idle desktop, empty native host and empty web-view host.
Report total cost and incremental scene cost; do not hide host overhead.

Use presentation timestamps rather than timer callbacks to calculate delivered
fps. Keep lightweight counters in timed runs and use OS CPU/GPU traces for
diagnosis. Record counter availability and sampling overhead. Whole-system power
differences require quiet matched baselines and repeated measurements. Do not
present CPU/GPU utilisation as measured watts. Mark unavailable metrics unmeasured.

## Folding impact

Measure this separately from recorded-data renderer trials. Include the case
where the screensaver shares the folding GPU, and the case where only the remote
viewer runs on another computer. Keep FAH settings and resource allocation fixed.

Use baseline/native/baseline/browser/baseline blocks within the same WU, then
repeat with the candidate order reversed if that WU lasts long enough. Each block
must cover at least five minutes and three comparable logged progress intervals;
extend it if checkpoints or slow progress leave too few samples. Exclude startup
and explicitly identify checkpoint/upload intervals. Do not compare unrelated WUs.

Compare progress time against adjacent baselines. Report throughput change,
variation, sample counts and temperatures/clocks where available. Treat a WU
change or thermal drift as a reason to repeat or mark the result inconclusive.
Do not infer a small throughput difference from estimated points per day alone.

## Host and failure checks

Check preview and full-screen launch; ten start/stop cycles; style and background
changes; mixed-resolution/mixed-refresh displays; lock/unlock; display sleep;
system sleep/wake; and display connection/removal. Verify that rendering and
reception stop when their host session ends, and memory does not grow per cycle.

Exercise connection loss/recovery, source restart, WU replacement, missing
topology, zero frames, malformed data and the largest supported frame history.
Keep these states visible. Do not substitute a demo protein or silently switch
renderer, adapter, background, resolution or quality to make a test pass.

## Proposed decision criteria and results

For the agreed 60 fps target, propose at least 99% of scheduled presentations
delivered during each steady-state trial, with p99 presentation intervals no
greater than 33.4 ms on a 60 Hz display. State the effective target when the
display refresh rate constrains it. Report all failures by model/style/scene.

Prefer the native candidate if it meets visual, lifecycle and pacing checks and
does not show a repeatable material resource or folding-throughput disadvantage.
If results overlap, report that uncertainty. If the browser wins, identify the
specific measured benefit and include its full host/process cost. Do not select
a winner from one metric, one platform or a reduced-quality scene.

For each trial save the candidate/build, OS, GPU/driver, display configuration,
fixture checksum, settings, environment, raw counters and exclusions. Summarise
the three trials and their range. Keep conclusions separate from measurements.
The initial release requires results on both platforms; an untested platform
must remain explicitly unverified.
