#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Update the cask from one published FAH Saver release, without extracting it."""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import re
import stat
import struct
import sys
import urllib.request
import zipfile


REPOSITORY = "le0-VV/FAH-saver"
HOMEPAGE = f"https://github.com/{REPOSITORY}"
CASK_URL = (HOMEPAGE + "/releases/download/v#{version}/"
            "FAHSaver-macos-arm64-v#{version}.zip")
MAX_ARCHIVE = 256 * 1024 * 1024
MAX_UNPACKED = 512 * 1024 * 1024
VERSION = r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"


def parse_version(value, prefix=""):
    if not isinstance(value, str) or len(value) > 128:
        raise ValueError("Expected a stable semantic version such as v1.0.0")
    match = re.fullmatch(re.escape(prefix) + VERSION, value, flags=re.ASCII)
    if not match:
        raise ValueError("Expected a stable semantic version such as v1.0.0")
    return tuple(int(part) for part in match.groups())


def asset_url(tag):
    parse_version(tag, "v")
    return f"{HOMEPAGE}/releases/download/{tag}/FAHSaver-macos-arm64-{tag}.zip"


def release_asset(release, tag):
    parse_version(tag, "v")
    if (not isinstance(release, dict) or release.get("tag_name") != tag
            or release.get("draft") is not False or release.get("prerelease") is not False
            or not isinstance(release.get("published_at"), str) or not release["published_at"]):
        raise ValueError("The requested tag must have a published, stable release")
    name = f"FAHSaver-macos-arm64-{tag}.zip"
    assets = release.get("assets")
    if not isinstance(assets, list):
        raise ValueError("The release has no asset list")
    matches = [item for item in assets if isinstance(item, dict) and item.get("name") == name]
    if len(matches) != 1:
        raise ValueError(f"Expected exactly one release asset named {name}")
    asset = matches[0]
    size = asset.get("size")
    if (asset.get("state") != "uploaded" or type(size) is not int or not 0 < size <= MAX_ARCHIVE
            or asset.get("browser_download_url") != asset_url(tag)):
        raise ValueError("Release asset URL, upload state or size is invalid")
    return asset


def fetch_release(tag):
    parse_version(tag, "v")
    headers = {"Accept": "application/vnd.github+json", "X-GitHub-Api-Version": "2022-11-28",
               "User-Agent": "FAH-Saver-Homebrew-Updater"}
    if os.environ.get("GH_TOKEN"):
        headers["Authorization"] = "Bearer " + os.environ["GH_TOKEN"]
    request = urllib.request.Request(
        f"https://api.github.com/repos/{REPOSITORY}/releases/tags/{tag}", headers=headers)
    with urllib.request.urlopen(request, timeout=60) as response:
        raw = response.read(2 * 1024 * 1024 + 1)
    if len(raw) > 2 * 1024 * 1024:
        raise ValueError("Release metadata exceeds the 2 MiB limit")
    return json.loads(raw)


def download_asset(asset):
    # The public download has no API token, including after GitHub redirects it.
    request = urllib.request.Request(asset["browser_download_url"],
                                     headers={"User-Agent": "FAH-Saver-Homebrew-Updater"})
    with urllib.request.urlopen(request, timeout=120) as response:
        data = response.read(asset["size"] + 1)
    if len(data) != asset["size"]:
        raise ValueError("Downloaded asset size differs from the release metadata")
    digest = hashlib.sha256(data).hexdigest()
    if asset.get("digest") not in (None, "sha256:" + digest):
        raise ValueError("Downloaded asset SHA-256 differs from the GitHub asset digest")
    return data, digest


def validate_macho(data):
    if len(data) < 32:
        raise ValueError("The saver executable has a truncated Mach-O header")
    magic, cpu, _, kind, commands, command_bytes, _, _ = struct.unpack_from("<8I", data)
    if magic != 0xFEEDFACF or cpu != 0x0100000C or kind != 8:
        raise ValueError("The saver executable must be a thin arm64 Mach-O bundle")
    end = 32 + command_bytes
    if end > len(data) or commands > command_bytes // 8:
        raise ValueError("The saver executable has invalid Mach-O load commands")
    offset = 32
    minimums = []
    for _ in range(commands):
        if offset + 8 > end:
            raise ValueError("The saver executable has truncated Mach-O load commands")
        command, size = struct.unpack_from("<2I", data, offset)
        if size < 8 or size % 8 or offset + size > end:
            raise ValueError("The saver executable has invalid Mach-O command bounds")
        if command == 0x32:  # LC_BUILD_VERSION
            if size < 24:
                raise ValueError("The saver executable has an invalid build version")
            platform, minimum = struct.unpack_from("<2I", data, offset + 8)
            if platform != 1:
                raise ValueError("The saver executable must target macOS")
            minimums.append(minimum)
        offset += size
    if offset != end or minimums != [0x000B0000]:
        raise ValueError("The saver executable must declare a macOS 11.0 deployment target")


def validate_archive(data, version):
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        entries = archive.infolist()
        names = [item.filename for item in entries]
        filesystem_names = [name.rstrip("/").casefold() for name in names]
        if (len(names) != len(set(filesystem_names))
                or sum(item.file_size for item in entries) > MAX_UNPACKED):
            raise ValueError("The ZIP contains duplicate entries or exceeds the unpacked size limit")
        for item in entries:
            path = PurePosixPath(item.filename)
            mode = item.external_attr >> 16
            if (path.is_absolute() or ".." in path.parts or "\\" in item.filename
                    or item.filename.rstrip("/") != str(path)
                    or "\x00" in item.filename or stat.S_ISLNK(mode)
                    or item.flag_bits & 1):
                raise ValueError("The ZIP contains an unsafe path, symbolic link or encrypted member")
        plist_name = "FAHSaver.saver/Contents/Info.plist"
        executable_name = "FAHSaver.saver/Contents/MacOS/FAHSaver"
        if plist_name not in names or executable_name not in names:
            raise ValueError("The ZIP must contain FAHSaver.saver at its root")
        if archive.getinfo(plist_name).file_size > 1024 * 1024:
            raise ValueError("The saver Info.plist exceeds the 1 MiB limit")
        info = plistlib.loads(archive.read(plist_name))
        expected = {"CFBundleShortVersionString": version, "CFBundleExecutable": "FAHSaver",
                    "CFBundleIdentifier": "org.fahsaver.screensaver", "CFBundlePackageType": "BNDL",
                    "LSMinimumSystemVersion": "11.0"}
        if not isinstance(info, dict) or any(info.get(key) != value for key, value in expected.items()):
            raise ValueError("The saver Info.plist version, identity or deployment target is incorrect")
        executable = archive.getinfo(executable_name)
        if executable.file_size > MAX_ARCHIVE or not executable.external_attr >> 16 & 0o111:
            raise ValueError("The saver executable is oversized or has no executable permission")
        validate_macho(archive.read(executable_name))


def new_cask(version, digest):
    return f'''cask "fah-saver" do
  version "{version}"
  sha256 "{digest}"

  url "{CASK_URL}"
  name "FAH Saver"
  desc "Folding@home protein model screensaver"
  homepage "{HOMEPAGE}"

  depends_on :macos
  depends_on arch: :arm64

  screen_saver "FAHSaver.saver"

  caveats <<~EOS
    This build is ad hoc signed and is not notarised.
    macOS may require explicit approval before it can run.

    If you previously installed FAH Saver manually, remove that copy from
    /Library/Screen Savers or ~/Library/Screen Savers before installing this cask.
    Keep your FAH Saver preferences when removing a manual copy.
  EOS
end
'''


def updated_cask(previous, version, digest):
    target = parse_version(version)
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise ValueError("Expected a SHA-256 digest")
    if previous is None:
        return new_cask(version, digest)
    versions = re.findall(r'^  version "([^"]+)"$', previous, re.MULTILINE)
    hashes = re.findall(r'^  sha256 "([0-9a-f]{64})"$', previous, re.MULTILINE)
    urls = re.findall(r'^  url "([^"]+)"$', previous, re.MULTILINE)
    if len(versions) != 1 or len(hashes) != 1 or urls != [CASK_URL]:
        raise ValueError("The existing cask must have one version, SHA-256 and expected release URL")
    current = parse_version(versions[0])
    if target < current:
        raise ValueError("Refusing to downgrade the Homebrew cask")
    if target == current and digest != hashes[0]:
        raise ValueError("Refusing to replace the asset hash for an existing cask version")
    result = re.sub(r'^  version "[^"]+"$', f'  version "{version}"', previous, flags=re.MULTILINE)
    return re.sub(r'^  sha256 "[0-9a-f]{64}"$', f'  sha256 "{digest}"', result, flags=re.MULTILINE)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True, help="Published stable release tag, e.g. v1.0.0")
    parser.add_argument("--cask", type=Path, required=True, help="Tap checkout's Casks/fah-saver.rb")
    parser.add_argument("--dry-run", action="store_true", help="Verify and print the cask without writing it")
    args = parser.parse_args(argv)
    parse_version(args.tag, "v")
    previous = args.cask.read_text(encoding="utf-8") if args.cask.exists() else None
    asset = release_asset(fetch_release(args.tag), args.tag)
    data, digest = download_asset(asset)
    version = args.tag[1:]
    validate_archive(data, version)
    result = updated_cask(previous, version, digest)
    if args.dry_run:
        print(result, end="")
    elif result != previous:
        args.cask.parent.mkdir(parents=True, exist_ok=True)
        args.cask.write_text(result, encoding="utf-8")
        print(f"Updated {args.cask} to {version} (SHA-256 {digest})")
    else:
        print(f"The cask already contains verified release {version}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, KeyError, zipfile.BadZipFile, plistlib.InvalidFileException) as error:
        print(f"Homebrew update failed: {error}", file=sys.stderr)
        sys.exit(1)
