# Building and publishing a release

A `vMAJOR.MINOR.PATCH` tag builds the macOS ARM64 bundle and Windows x64 packages.
The workflow creates a **draft** GitHub release. Check the exact downloaded packages
on the target systems, then publish the draft. Publication updates the Homebrew tap.

The source version in `CMakeLists.txt`, the tag, the macOS bundle version and the
Windows file/product version must agree. The packaging script rejects a mismatch.
Commit the version, notices and release notes before tagging.
Local packaging requires the release tag to exist and point to the current commit. Use a new tag for
changed source; do not move an existing published tag.

## Toolchain

The automated build uses the public ARM64 `macos-15` runner. It sets the macOS
architecture to `arm64` and the deployment target to macOS 11.0. Intel Macs are not
part of this release contract. Apple SDK and Homebrew tool revisions can change
between runner images, so this is a repeatable build procedure, not a guarantee of
byte-identical compiled binaries across different runner images.

Windows builds use Zig 0.17.1 and Vulkan-Headers 1.4.363. The bootstrap script pins
both official download URLs and SHA-256 hashes. It generates a small import library
for the Vulkan entry points used by FAH Saver; the Vulkan loader remains supplied
by the user's graphics driver. The committed export list contains API names only.
The bootstrap cache and generated wrappers can move together to another path.

NSIS must report `v3.13`. The workflow stops if Homebrew supplies another version.
Review the package notices before changing Zig, Vulkan headers or NSIS. CMake,
Ninja, glslang and the Apple command-line tools must also be installed. No Windows
SDK, MSVC runtime or browser runtime is bundled.

## Local build on an ARM64 Mac

Run native ARM64 tools. If the terminal itself runs under Rosetta, prefix each
command with `/usr/bin/arch -arm64` and use an ARM64 Python installation.

```sh
brew install cmake ninja glslang nsis
python3 -m unittest discover -s tools/tests -v
python3 platform/windows/installer/test_packager.py -v

cmake -S . -B build/release-macos -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DBUILD_TESTING=ON
cmake --build build/release-macos --parallel 3 --target \
  FAHSaver fah_core_tests fah_snapshot_cache_tests fah_live_model_tests \
  fah_decimal_input_tests fah_source_tests fah_thread_sample
ctest --test-dir build/release-macos --output-on-failure \
  -R '^(core|snapshot_cache|live_model|decimal_input|source_protocol|thread_attribution_logic)$'

python3 tools/bootstrap-windows.py --cache build/release-tools
cmake -S . -B build/release-windows -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/windows-zig.cmake" \
  -DFAH_WINDOWS_TOOLS="$PWD/build/release-tools" \
  -DFAH_GLSLANG_VALIDATOR="$(command -v glslangValidator)"
cmake --build build/release-windows --parallel 3 --target \
  fah_saver_windows fah_windows_options_tests fah_windows_installer_lifecycle

python3 tools/release.py package --tag v1.0.0 \
  --mac-bundle build/release-macos/platform/macos/FAHSaver.saver \
  --windows-build build/release-windows/platform/windows \
  --output build/release-assets
python3 tools/release.py verify --tag v1.0.0 --output build/release-assets
```

The output directory must be empty. The bootstrap accepts `--zig-archive` and
`--vulkan-archive` to reuse downloaded archives, and verifies their pinned hashes.
Do not use these options to substitute a different toolchain.

## Release assets

The GitHub draft or published release exposes only these two uploaded downloads:

| Download | Contents |
| --- | --- |
| `FAHSaver-macos-arm64-v1.0.0.zip` | macOS ARM64 `FAHSaver.saver` bundle and manifest |
| `FAHSaver-Setup.exe` | Per-user Windows x64 installer |

The Mac ZIP filename includes the release tag. Homebrew keeps its existing
versioned, tag-pinned download contract, for example
`https://github.com/le0-VV/FAH-saver/releases/download/v1.0.0/FAHSaver-macos-arm64-v1.0.0.zip`,
and records that download's SHA-256. Windows uses the canonical
`FAHSaver-Setup.exe` filename in each release.

Corresponding source and build scripts remain in the public tagged repository.
GitHub also provides its automatic **Source code (zip)** and **Source code (tar.gz)**
links for the tag.

### Internal CI verification files

The packaging script still creates and verifies the full set below. The
`release-assets` Actions artifact retains these files for acceptance checks and
provenance. Only the two platform downloads above are uploaded to the GitHub
release; the other packages, standalone manifests, source archive and checksums
stay in the CI artifact.

For version `1.0.0`, the internal output contains:

| File | Contents |
| --- | --- |
| `FAHSaver-macos-arm64-v1.0.0.zip` | `FAHSaver.saver` at the archive root and `manifest.json` |
| `FAHSaver-macos-arm64-v1.0.0.manifest.json` | Bundle file hashes, source revision and platform information |
| `FAHSaver-windows-x64-v1.0.0.zip` | Portable `.scr`, identical preview `.exe`, four shaders, licence/source notices and manifest |
| `FAHSaver-windows-x64-v1.0.0.manifest.json` | Portable payload hashes, source revision and platform information |
| `FAHSaver-Setup-1.0.0.exe` | Per-user Windows installer |
| `FAHSaver-Setup-1.0.0.manifest.json` | Installer hash and checked installed-file hashes |
| `FAHSaver-source-v1.0.0.tar.gz` | Corresponding committed source, including build and installation scripts |
| `FAHSaver-macos-arm64.zip` | Byte-identical alias for the versioned macOS ZIP |
| `FAHSaver-Setup.exe` | Byte-identical alias for the versioned Windows installer |
| `SHA256SUMS` | SHA-256 hashes for all nine assets above |

The macOS bundle is ad hoc signed and is not notarised. The Windows executables
are unsigned. Keep the installation guidance consistent with these facts. The
release tools do not remove quarantine attributes.

## GitHub and Homebrew setup

The source repository needs Actions enabled with permission to create releases.
Only the final draft job has `contents: write`; the build job has `contents: read`.
The release workflow uses commit-pinned official checkout and artifact actions.
It does not run GPU previews or alter a workstation's screen saver registration.

The Homebrew updater requires a separate `homebrew-fah-saver` repository and a
write-enabled SSH deploy key scoped to that tap. Store its private key only in the
source repository's `TAP_DEPLOY_KEY` Actions secret. The public key belongs in the
tap repository's deploy-key settings. Do not put a private key in either repository.

Publish a reviewed draft through GitHub or a user-authenticated `gh release edit
TAG --draft=false`. A `GITHUB_TOKEN` action that publishes a release does not
trigger another release workflow. If automatic publication is added later, its
workflow must explicitly call the reusable `update-homebrew.yml` workflow with
`release_tag` and `TAP_DEPLOY_KEY` after publication.

The updater downloads the published macOS ZIP, checks its bundle metadata and
ARM64 target, calculates the cask SHA-256 and commits the updated cask to the tap.
A new release therefore updates Homebrew without a hand-edited version or hash.
A failed tap update can be retried with the updater's manual workflow dispatch.

Native Windows installer acceptance still needs the manual lifecycle helper on a
Windows desktop. A cross-build checks the PE resource, payload, shaders and
installer compilation; it does not prove Windows runtime behaviour. The Mac
system screen saver host also needs a real preview/start/exit check before
publication.
