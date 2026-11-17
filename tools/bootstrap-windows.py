#!/usr/bin/env python3
"""Fetch the pinned Windows cross-build tools for an ARM64 macOS host."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
ZIG_VERSION = "0.17.1"
VULKAN_VERSION = "1.4.363"
DOWNLOADS = {
    "zig": {
        "url": f"https://ziglang.org/download/{ZIG_VERSION}/zig-aarch64-macos-{ZIG_VERSION}.tar.xz",
        "sha256": "75f1bc7a1e9852f17ec9a0c1895f767c4ec56d46bc8a834920eece1d0359dd8d",
        "directory": f"zig-aarch64-macos-{ZIG_VERSION}",
    },
    "vulkan": {
        "url": f"https://codeload.github.com/KhronosGroup/Vulkan-Headers/tar.gz/refs/tags/v{VULKAN_VERSION}",
        "sha256": "cbaf687d3c59b9666fe080e7f8396b6b4f4d344a768ee6b337c59852f4526b68",
        "directory": f"Vulkan-Headers-{VULKAN_VERSION}",
    },
}


def sha256(path):
    with path.open("rb") as stream:
        digest = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
        return digest.hexdigest()


def checked_archive(cache, name, supplied=None):
    specification = DOWNLOADS[name]
    archive = supplied or cache / specification["url"].rsplit("/", 1)[-1]
    if not archive.is_file():
        if supplied:
            raise ValueError(f"Missing supplied {name} archive: {archive}")
        temporary = archive.with_suffix(archive.suffix + ".part")
        request = urllib.request.Request(specification["url"], headers={"User-Agent": "FAH-Saver-release"})
        with urllib.request.urlopen(request, timeout=120) as source, temporary.open("wb") as target:
            shutil.copyfileobj(source, target)
        if sha256(temporary) != specification["sha256"]:
            temporary.unlink()
            raise ValueError(f"Downloaded {name} archive failed SHA-256 verification")
        temporary.replace(archive)
    if sha256(archive) != specification["sha256"]:
        raise ValueError(f"Cached {name} archive failed SHA-256 verification")
    return archive


def extract_checked(archive, cache, specification):
    destination = cache / specification["directory"]
    marker = destination / ".fah-archive-sha256"
    if destination.exists():
        if marker.is_file() and marker.read_text().strip() == specification["sha256"]:
            return destination
        raise ValueError(f"Unverified tool directory already exists: {destination}")
    with tempfile.TemporaryDirectory(dir=cache, prefix="extract-") as temporary:
        base = Path(temporary)
        with tarfile.open(archive) as source:
            members = source.getmembers()
            for member in members:
                path = Path(member.name)
                if (path.is_absolute() or ".." in path.parts or
                        not path.parts or path.parts[0] != specification["directory"] or
                        not (member.isfile() or member.isdir())):
                    raise ValueError("Tool archive contains an unsafe path or link")
            source.extractall(base, members=members)
        extracted = base / specification["directory"]
        (extracted / marker.name).write_text(specification["sha256"] + "\n")
        extracted.rename(destination)
    return destination


def write_wrappers(cache):
    binary = cache / "bin"
    binary.mkdir(exist_ok=True)
    prefix = ('#!/bin/sh\nset -eu\n'
              'base="$(CDPATH= cd "$(dirname "$0")/.." && pwd)"\n'
              'export ZIG_GLOBAL_CACHE_DIR="$base/zig-cache"\n')
    commands = {
        "zig-cxx": 'c++ -target x86_64-windows-gnu',
        "zig-ar": 'ar',
        "zig-ranlib": 'ranlib',
        "zig-rc": f'rc /:auto-includes none /i "$base/zig-aarch64-macos-{ZIG_VERSION}/lib/libc/include/any-windows-any"',
    }
    for name, command in commands.items():
        path = binary / name
        path.write_text(prefix + f'exec "$base/zig-aarch64-macos-{ZIG_VERSION}/zig" {command} "$@"\n')
        path.chmod(0o755)


def bootstrap(cache, zig_archive=None, vulkan_archive=None):
    cache = cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    for name, supplied in (("zig", zig_archive), ("vulkan", vulkan_archive)):
        archive = checked_archive(cache, name, supplied)
        extract_checked(archive, cache, DOWNLOADS[name])
    zig = cache / DOWNLOADS["zig"]["directory"] / "zig"
    if subprocess.check_output([str(zig), "version"], text=True).strip() != ZIG_VERSION:
        raise ValueError("The extracted Zig version does not match the pinned version")
    write_wrappers(cache)
    subprocess.run([str(zig), "dlltool", "-m", "i386:x86-64", "-d", str(ROOT / "cmake/vulkan.def"),
                    "-l", str(cache / "libvulkan-1.a")], check=True)
    report = {"zig": ZIG_VERSION, "vulkan_headers": VULKAN_VERSION,
              "archives": {name: {"url": item["url"], "sha256": item["sha256"]}
                           for name, item in DOWNLOADS.items()}}
    (cache / "toolchain.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, default=ROOT / "build/release-tools")
    parser.add_argument("--zig-archive", type=Path)
    parser.add_argument("--vulkan-archive", type=Path)
    args = parser.parse_args()
    bootstrap(args.cache, args.zig_archive, args.vulkan_archive)
    print(f"Windows x64 toolchain ready: {args.cache.resolve()}")


if __name__ == "__main__":
    main()
