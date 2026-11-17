#!/usr/bin/env python3
"""Check and package the tagged macOS ARM64 and Windows x64 releases."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import re
import shutil
import struct
import subprocess
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = "https://github.com/le0-VV/FAH-saver"
PROJECT_NOTICES = ("LICENSE", "COPYRIGHT", "THIRD-PARTY-NOTICES.md")


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def version_from_tag(tag):
    if not re.fullmatch(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", tag):
        raise ValueError("Release tag must be vMAJOR.MINOR.PATCH with numeric components")
    version = tag[1:]
    if any(int(part) > 65535 for part in version.split(".")):
        raise ValueError("Release version must fit the Windows version resource")
    return version


def check_version(tag):
    version = version_from_tag(tag)
    match = re.search(r"project\(FAHSaver VERSION ([0-9.]+)\b", (ROOT / "CMakeLists.txt").read_text())
    if not match or match[1] != version:
        raise ValueError("Release tag does not match CMake PROJECT_VERSION")
    return version


def source_record(tag):
    if subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=no"], cwd=ROOT, text=True).strip():
        raise ValueError("Commit tracked source changes before packaging a release")
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    tagged = subprocess.check_output(["git", "rev-parse", f"refs/tags/{tag}^{{commit}}"], cwd=ROOT, text=True).strip()
    if tagged != commit:
        raise ValueError("The release tag must point to the checked-out source commit")
    return {"repository": REPOSITORY, "tag": tag, "commit": commit,
            "url": f"{REPOSITORY}/tree/{tag}",
            "archive_url": f"{REPOSITORY}/archive/refs/tags/{tag}.tar.gz"}


def source_text(version):
    return (ROOT / "cmake/SOURCE.txt.in").read_text().replace("@PROJECT_VERSION@", version)


def checked_file(path, base):
    if not path.is_file() or path.is_symlink() or not path.resolve().is_relative_to(base.resolve()):
        raise ValueError(f"Missing or linked release file: {path.name}")
    return path


def file_hashes(base):
    result = {}
    for path in sorted(base.rglob("*")):
        if path.is_symlink():
            raise ValueError("Release payload cannot contain symbolic links")
        if path.is_file():
            result[path.relative_to(base).as_posix()] = sha256(path)
    return result


def write_manifest(stage, version, platform, source, build):
    manifest = {"product": "FAH Saver", "version": version, "platform": platform,
                "source": source, "build": build, "files": file_hashes(stage)}
    (stage / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def write_zip(stage, output, epoch):
    # Stable entry order and timestamps keep packaging independent of file copy times.
    timestamp = time.gmtime(max(epoch, 315532800))[:6]
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(stage.rglob("*")):
            if path.is_symlink():
                raise ValueError("Release archive cannot contain symbolic links")
            if path.is_file():
                info = zipfile.ZipInfo.from_file(path, path.relative_to(stage).as_posix())
                info.date_time = timestamp
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, path.read_bytes(), compresslevel=9)
    with zipfile.ZipFile(output) as archive:
        if archive.testzip():
            raise ValueError("Release ZIP failed its CRC check")
        expected = file_hashes(stage)
        actual = {name: hashlib.sha256(archive.read(name)).hexdigest() for name in archive.namelist()}
        if actual != expected:
            raise ValueError("Release ZIP does not match the staged payload")


def check_macos_binary(path):
    data = path.read_bytes()
    if len(data) < 32 or struct.unpack_from("<III", data) != (0xFEEDFACF, 0x0100000C, 0):
        raise ValueError("macOS release requires a thin ARM64 Mach-O binary")
    if struct.unpack_from("<I", data, 12)[0] != 8:
        raise ValueError("macOS screen saver must be an MH_BUNDLE binary")
    offset, minimum = 32, None
    for _ in range(struct.unpack_from("<I", data, 16)[0]):
        if offset + 8 > len(data):
            raise ValueError("Truncated Mach-O load command")
        command, size = struct.unpack_from("<II", data, offset)
        if size < 8 or offset + size > len(data):
            raise ValueError("Invalid Mach-O load command")
        if command == 0x32 and size >= 24:
            platform, minimum = struct.unpack_from("<II", data, offset + 8)
            if platform != 1:
                raise ValueError("Release binary targets a platform other than macOS")
        elif command == 0x24 and size >= 16:
            minimum = struct.unpack_from("<I", data, offset + 8)[0]
        offset += size
    if minimum != 11 << 16:
        raise ValueError("macOS release binary must target macOS 11.0")


def check_windows_binary(path, version):
    data = path.read_bytes()
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("Expected a Windows PE executable")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("Invalid Windows PE header")
    machine, sections = struct.unpack_from("<HH", data, pe + 4)
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    if machine != 0x8664 or optional + optional_size > len(data) or struct.unpack_from("<H", data, optional)[0] != 0x20B:
        raise ValueError("Windows release requires an x64 PE32+ executable")
    table = optional + optional_size

    def file_offset(rva):
        for index in range(sections):
            section = table + index * 40
            if section + 40 > len(data):
                break
            size, start, raw_size, raw = struct.unpack_from("<IIII", data, section + 8)
            if start <= rva < start + max(size, raw_size):
                result = raw + rva - start
                if result < len(data):
                    return result
        raise ValueError("Invalid Windows version resource address")

    if optional_size < 112 + 24:
        raise ValueError("Missing Windows resource data directory")
    resource_rva = struct.unpack_from("<I", data, optional + 112 + 16)[0]
    resource = file_offset(resource_rva)

    def resource_entry(offset, wanted=None):
        if offset + 16 > len(data):
            raise ValueError("Truncated Windows resource directory")
        count = sum(struct.unpack_from("<HH", data, offset + 12))
        for index in range(count):
            location = offset + 16 + index * 8
            if location + 8 > len(data):
                break
            key, child = struct.unpack_from("<II", data, location)
            if wanted is None or key == wanted:
                return child
        raise ValueError("Missing Windows version resource")

    child = resource_entry(resource, 16)
    for _ in range(2):
        if not child & 0x80000000:
            raise ValueError("Invalid Windows version resource tree")
        child = resource_entry(resource + (child & 0x7FFFFFFF))
    if child & 0x80000000 or resource + child + 16 > len(data):
        raise ValueError("Invalid Windows version resource leaf")
    rva, size = struct.unpack_from("<II", data, resource + child)
    value = data[file_offset(rva):file_offset(rva) + size]
    key = "VS_VERSION_INFO\0".encode("utf-16le")
    if value[6:6 + len(key)] != key:
        raise ValueError("Invalid Windows fixed version information")
    start = (6 + len(key) + 3) & ~3
    if start + 24 > len(value) or struct.unpack_from("<I", value, start)[0] != 0xFEEF04BD:
        raise ValueError("Missing Windows fixed version information")
    major, minor, patch = (int(part) for part in version.split("."))
    expected = (major << 16 | minor, patch << 16)
    if struct.unpack_from("<II", value, start + 8) != expected or struct.unpack_from("<II", value, start + 16) != expected:
        raise ValueError("Windows file/product version does not match the release tag")


def stage_macos(bundle, stage, version, source):
    if bundle.name != "FAHSaver.saver" or bundle.is_symlink():
        raise ValueError("Expected an unlinked FAHSaver.saver bundle")
    file_hashes(bundle)
    target = stage / bundle.name
    shutil.copytree(bundle, target)
    contents = target / "Contents"
    with checked_file(contents / "Info.plist", stage).open("rb") as stream:
        info = plistlib.load(stream)
    if (info.get("CFBundleShortVersionString") != version or
            info.get("CFBundleIdentifier") != "org.fahsaver.screensaver" or
            info.get("CFBundleExecutable") != "FAHSaver" or info.get("CFBundlePackageType") != "BNDL"):
        raise ValueError("macOS bundle metadata does not match the release")
    check_macos_binary(checked_file(contents / "MacOS/FAHSaver", stage))
    resources = contents / "Resources"
    for name in (*PROJECT_NOTICES, "nlohmann-json-LICENSE.MIT"):
        checked_file(resources / name, stage)
    if checked_file(resources / "SOURCE.txt", stage).read_text() != source_text(version):
        raise ValueError("macOS bundle source notice does not match the release tag")
    subprocess.run(["/usr/bin/codesign", "--force", "--deep", "--sign", "-", str(target)], check=True)
    subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(target)], check=True)
    return write_manifest(stage, version, "macos-arm64", source,
                          {"minimum_macos": "11.0", "signature": "ad hoc; not notarised"})


def stage_windows(build, stage, version, source):
    names = json.loads((ROOT / "platform/windows/installer/payload-files.json").read_text())
    for name in names:
        relative = Path(name)
        if relative.is_absolute() or ".." in relative.parts or "\\" in name:
            raise ValueError("Invalid installer payload inventory path")
        target = stage / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if name == "SOURCE.txt":
            target.write_text(source_text(version))
            continue
        if name in PROJECT_NOTICES:
            origin = ROOT / name
        elif name == "nlohmann-json-LICENSE.MIT":
            origin = ROOT / "third_party/nlohmann/LICENSE.MIT"
        elif relative.parts[0] == "licences":
            origin = ROOT / "third_party/licenses" / relative.name
        else:
            origin = build / relative
        shutil.copy2(checked_file(origin, ROOT if origin.is_relative_to(ROOT) else build), target)
    check_windows_binary(stage / "FAHSaver.scr", version)
    preview = checked_file(build / "fah-preview.exe", build)
    if sha256(preview) != sha256(stage / "FAHSaver.scr"):
        raise ValueError("Windows preview executable differs from FAHSaver.scr")
    shutil.copy2(preview, stage / "fah-preview.exe")
    return write_manifest(stage, version, "windows-x64", source,
                          {"renderer": "Vulkan", "signature": "unsigned"})


def package(tag, bundle, windows, output, makensis):
    version = check_version(tag)
    source = source_record(tag)
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError("Release output directory must be empty")
    epoch = int(subprocess.check_output(["git", "show", "-s", "--format=%ct", "HEAD"], cwd=ROOT, text=True))
    with tempfile.TemporaryDirectory(prefix="fah-release-") as temporary:
        base = Path(temporary)
        for platform, payload in (("macos-arm64", bundle), ("windows-x64", windows)):
            stage = base / platform
            stage.mkdir()
            manifest = (stage_macos(payload.resolve(strict=True), stage, version, source)
                        if platform == "macos-arm64" else
                        stage_windows(payload.resolve(strict=True), stage, version, source))
            stem = f"FAHSaver-{platform}-v{version}"
            write_zip(stage, output / f"{stem}.zip", epoch)
            (output / f"{stem}.manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            if platform == "windows-x64":
                spec = importlib.util.spec_from_file_location("installer_packager", ROOT / "tools/package-windows-installer.py")
                packager = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(packager)
                packager.build(stage, output / f"FAHSaver-Setup-{version}.exe", makensis)
    with (output / f"FAHSaver-source-v{version}.tar.gz").open("wb") as stream:
        subprocess.run(["git", "archive", "--format=tar.gz", f"--prefix=FAH-saver-{version}/", "HEAD"], cwd=ROOT, stdout=stream, check=True)
    shutil.copy2(output / f"FAHSaver-macos-arm64-v{version}.zip", output / "FAHSaver-macos-arm64.zip")
    shutil.copy2(output / f"FAHSaver-Setup-{version}.exe", output / "FAHSaver-Setup.exe")
    checksums = "".join(f"{sha256(path)}  {path.name}\n" for path in sorted(output.iterdir()))
    (output / "SHA256SUMS").write_text(checksums)
    verify(output, tag)


def verify(output, tag):
    version = version_from_tag(tag)
    expected = {f"FAHSaver-{platform}-v{version}.{suffix}"
                for platform in ("macos-arm64", "windows-x64") for suffix in ("zip", "manifest.json")}
    expected |= {f"FAHSaver-Setup-{version}.exe", f"FAHSaver-Setup-{version}.manifest.json", f"FAHSaver-source-v{version}.tar.gz",
                 "FAHSaver-macos-arm64.zip", "FAHSaver-Setup.exe"}
    entries = {}
    for line in (output / "SHA256SUMS").read_text().splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([A-Za-z0-9._-]+)", line)
        if not match or match[2] in entries:
            raise ValueError("Invalid or duplicate SHA256SUMS entry")
        entries[match[2]] = match[1]
    if set(entries) != expected or {path.name for path in output.iterdir()} != expected | {"SHA256SUMS"}:
        raise ValueError("Release asset inventory does not match the tag")
    for name, digest in entries.items():
        if sha256(checked_file(output / name, output)) != digest:
            raise ValueError(f"Release asset checksum mismatch: {name}")
    for alias, name in (("FAHSaver-macos-arm64.zip", f"FAHSaver-macos-arm64-v{version}.zip"),
                        ("FAHSaver-Setup.exe", f"FAHSaver-Setup-{version}.exe")):
        if entries[alias] != entries[name]:
            raise ValueError(f"Canonical release asset differs from its versioned asset: {alias}")
    return entries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("check-version")
    check.add_argument("--tag", required=True)
    create = commands.add_parser("package")
    create.add_argument("--tag", required=True)
    create.add_argument("--mac-bundle", required=True, type=Path)
    create.add_argument("--windows-build", required=True, type=Path)
    create.add_argument("--output", required=True, type=Path)
    create.add_argument("--makensis", default="makensis")
    validate = commands.add_parser("verify")
    validate.add_argument("--tag", required=True)
    validate.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.command == "check-version":
        print(check_version(args.tag))
    elif args.command == "package":
        package(args.tag, args.mac_bundle, args.windows_build, args.output, args.makensis)
        print(f"Checked release assets: {args.output}")
    else:
        print(f"Verified {len(verify(args.output, args.tag))} assets")


if __name__ == "__main__":
    main()
