# Attribute Windows Vulkan worker CPU

`tools/windows-thread-sample.cpp` is a process-specific diagnostic. It does not
change the screensaver, drivers or folding settings. The coordinator must target
only the test process it launched. It requires the exact PID, absolute executable
path and creation FILETIME. A reused PID or a different executable is rejected.
Only native x64 Windows targets are supported.

The utility takes two thread-only PSS snapshots 1.5 seconds apart. It reports
user/kernel CPU deltas for matching thread lifetimes and maps each thread's
starting function to a module basename and relative virtual address (RVA).
PSS exposes these fields through `PSS_THREAD_ENTRY`.
[Thread information](https://learn.microsoft.com/en-us/windows/win32/api/processsnapshot/ns-processsnapshot-pss_thread_entry),
[snapshot capture](https://learn.microsoft.com/en-us/windows/win32/api/processsnapshot/nf-processsnapshot-psscapturesnapshot).

Module enumeration uses only the supplied process handle. No system-wide
process enumeration, ETW session, profiler installation or symbol download is
needed. [Module enumeration](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-enumprocessmodulesex).

## Build and self-check

From the repository root, the pure CPU-delta, process/thread identity and module
range checks also run on macOS:

```sh
clang++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Ithird_party tools/windows-thread-sample.cpp -o /tmp/fah-thread-self-check
/tmp/fah-thread-self-check --self-check
```

Cross-build with the existing Zig toolchain, using a writable cache directory:

```sh
/path/to/zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -D_WIN32_WINNT=0x0a00 -municode -Ithird_party tools/windows-thread-sample.cpp -lpsapi -o /tmp/fah-thread-sample.exe
```

## One coordinated Windows run

Use 64-bit PowerShell and the PID returned when launching the bounded renderer
test. Wait until its Vulkan window has started, so initialisation workers are
present in both snapshots. This example reads only that process:

```powershell
$testProcess = Get-Process -Id 1234 -ErrorAction Stop
$expectedExe = $testProcess.Path
$created = $testProcess.StartTime.ToFileTimeUtc().ToString()
& .\fah-thread-sample.exe --pid $testProcess.Id --exe $expectedExe --created $created
if ($LASTEXITCODE) { throw 'Thread attribution failed' }
```

An optional `--contexts` pass records the hottest measured thread's instruction
address as module basename and RVA. It takes at most 24 snapshots within a
two-second scheduling budget, at varying intervals to reduce frame-rate aliasing.
If the coordinator knows the GUI thread ID for this same process, pass
`--main-thread TID` to exclude it. Without that argument the hottest thread is
selected without assuming which thread is the GUI thread.

PSS captures control contexts for the target process's threads; the utility reads
and emits only the selected thread's instruction address. It emits no stack bytes
or other registers. It requests no VA clone or memory contents, and it never
calls `SuspendThread`.
Snapshots, walk markers and process handles are released on success and errors.
[Capture flags](https://learn.microsoft.com/en-us/windows/win32/api/processsnapshot/ne-processsnapshot-pss_capture_flags).

Give the test process enough lifetime for the 1.5-second CPU interval and optional
two-second sample pass. The coordinator should apply an external timeout, such
as eight seconds, to this helper. Individual Windows calls have no timeout, so
the scheduling budget is not a hard OS latency guarantee. Unsupported APIs,
access failures and a target that exits during sampling produce a nonzero exit.

## What the output establishes

- CPU deltas identify which matching thread lifetimes consumed CPU. Threads
  created or lost between snapshots can be omitted; counts are included.
  The reported wall span includes snapshot collection time. It is not an exact
  common counter boundary for every thread.
- A starting module identifies a thread entrypoint. It does not show where that
  thread spent its CPU time.
- Context samples identify observed user-mode instruction locations. They are
  wall-time samples, not CPU-weighted stack samples. They do not identify kernel
  hotspots, and a wait routine appearing often does not prove busy-waiting.
- Module maps are refreshed for context samples. Unmapped addresses have null
  module/RVA fields. DLL load/unload races still limit exact attribution.
- Context capture can perturb the test process. Record its overhead and use this
  run for attribution, not final frame-rate, power or CPU comparisons.

Keep the JSON private with the other diagnostic results. No FAH process is opened
or controlled by the intended coordinator invocation.
