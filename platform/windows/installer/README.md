# Windows setup build

The NSIS setup installs FAH Saver for the current Windows account. It needs no
administrator access. The default location is
`%LOCALAPPDATA%\Programs\FAH Saver`. Users download one Setup.exe, run it, and
can then delete the download. Later setups update the same managed location.

Setup selects the installed `.scr`, enables screen saving and creates an Apps
uninstall entry. It does not write the idle timeout, password requirement,
`Software\FAHSaver` preferences or model cache. The Finish page can open the
normal FAH Saver options panel. Existing manual installations are superseded
through registration; their old folders are not deleted.

Uninstall removes only the listed payload and its own uninstaller. It removes
its registration and disables screen saving only if the current selected saver
still identifies this installation. Quoted paths and Windows 8.3 paths are
normalised for this ownership check. Another selected saver is left unchanged.
Unknown files in the application folder are retained.

## Build and checks

Use Python 3.9 or later and NSIS (`makensis`; tested with 3.13). First build the
Windows release and assemble its hash-verified payload folder and manifest.
Then run from the repository root:

```sh
python3 tools/package-windows-installer.py \
  --payload build/package-windows-v0.1.10 \
  --output build/deliverables/FAHSaver-Setup-0.1.10.exe
python3 platform/windows/installer/test_packager.py -v
```

The build wrapper verifies the release version, x64 saver header, required
payload inventory and file hashes before calling NSIS. It writes an adjacent
`.manifest.json` with setup and payload hashes. The single inventory in
`payload-files.json` generates extraction, backup, copy and removal commands;
`fah-preview.exe` is a development tool and is not installed.

Setup first extracts into its private temporary folder. It preflights existing
files for locks, backs them up, and checks every copy. Copy or registration
failure triggers immediate file/registration rollback. An incomplete rollback
is reported explicitly. Silent errors return a nonzero exit code; lock failure
returns 2 without changing the installed files. Setup and uninstall share a
session-local mutex. Neither kills running processes or requests a reboot.

Native Windows lifecycle tests must still run before release. The test should
use `/S /D=<owned temporary folder>` with `/D` last, and use normal
`Uninstall.exe /S` without `_?=` so NSIS can copy its uninstaller to temporary
storage. Wait for actual removal, not just the initial launcher exit. Check
install, update, locked-file failure, quoted/8.3 paths, preservation of another
selected saver, and preservation of preferences/cache/unknown files. Save and
restore the affected current-user registry state around the test.

The installer owns `HKCU\Software\FAHSaver\Installer` and
`HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\FAHSaver` in the
64-bit view. Its only Desktop writes are `SCRNSAVE.EXE` and the active state
through `SystemParametersInfoW`; it leaves timeout and security values alone.

References: [NSIS scripting](https://nsis.sourceforge.io/Docs/Chapter4.html),
[Windows screen saver registration](https://learn.microsoft.com/en-us/windows/win32/devnotes/scrnsave-exe),
[SystemParametersInfoW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-systemparametersinfow).


### Silent failure codes

Silent mode retains specific failure stages for native integration diagnostics:
2 is a locked managed file; 10–14 are initialisation/platform, mutex, path,
downgrade and screen-saver-state checks; 20–27 are uninstaller preparation,
backup-folder creation, backup copy, installation-folder creation, payload copy,
registration, activation and uninstall-metadata writes; 30–34 are uninstall
ownership, deactivation, registration removal, file removal and metadata removal.
An activation/state API error also reports its immediate Windows error code.
The installer does not substitute a default state when those APIs fail.

On error, setup writes `%TEMP%\FAHSaver-Setup.log` in UTF-16. It contains the
failure stage, immediate native error where available, affected relative file,
source/destination and installer folders. The copy diagnostic is saved before
rollback can change registers. It contains no saved settings, source URL, model
data or cache contents. Successful setup does not write this diagnostic file.
