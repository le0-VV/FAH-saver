"""Release boundary checks; no GUI or network access."""

import importlib.util
import io
from pathlib import Path
import struct
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


release = load("release", "release.py")
bootstrap = load("bootstrap", "bootstrap-windows.py")


def pe_with_version(version):
    """One PE32+ .rsrc section, with a three-level RT_VERSION tree."""
    data = bytearray(2048)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 64)
    data[64:68] = b"PE\0\0"
    struct.pack_into("<HH", data, 68, 0x8664, 1)
    struct.pack_into("<H", data, 84, 240)
    struct.pack_into("<H", data, 88, 0x20B)
    struct.pack_into("<II", data, 88 + 112 + 16, 0x1000, 1024)
    section = 88 + 240
    data[section:section + 8] = b".rsrc\0\0\0"
    struct.pack_into("<IIII", data, section + 8, 1024, 0x1000, 1024, 512)
    for offset, key, child in ((512, 16, 0x80000020), (544, 1, 0x80000040), (576, 1033, 96)):
        struct.pack_into("<H", data, offset + 14, 1)
        struct.pack_into("<II", data, offset + 16, key, child)
    value = bytearray(128)
    key = "VS_VERSION_INFO\0".encode("utf-16le")
    value[6:6 + len(key)] = key
    start = (6 + len(key) + 3) & ~3
    major, minor, patch = version
    struct.pack_into("<IIIIII", value, start, 0xFEEF04BD, 0x10000,
                     major << 16 | minor, patch << 16, major << 16 | minor, patch << 16)
    struct.pack_into("<II", data, 608, 0x1100, len(value))
    data[768:896] = value
    return data


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="fah-release-tests-")
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)

    def test_tag_and_cmake_version_must_match(self):
        (self.base / "CMakeLists.txt").write_text("project(FAHSaver VERSION 1.2.3 LANGUAGES CXX)")
        with patch.object(release, "ROOT", self.base):
            self.assertEqual(release.check_version("v1.2.3"), "1.2.3")
            with self.assertRaisesRegex(ValueError, "does not match"):
                release.check_version("v1.2.4")
        for tag in ("1.2.3", "v01.2.3", "v1.2.3-rc1", "v65536.0.0", "v1.2.3\n"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                release.version_from_tag(tag)

    def test_packaging_requires_clean_exact_tagged_source(self):
        with patch.object(release.subprocess, "check_output", side_effect=["", "a" * 40, "a" * 40]):
            self.assertEqual(release.source_record("v1.2.3")["commit"], "a" * 40)
        with patch.object(release.subprocess, "check_output", side_effect=["", "a" * 40, "b" * 40]):
            with self.assertRaisesRegex(ValueError, "checked-out source commit"):
                release.source_record("v1.2.3")
        with patch.object(release.subprocess, "check_output", return_value=" M CMakeLists.txt"):
            with self.assertRaisesRegex(ValueError, "Commit tracked source"):
                release.source_record("v1.2.3")

    def test_windows_embedded_version_and_architecture(self):
        binary = self.base / "FAHSaver.scr"
        binary.write_bytes(pe_with_version((1, 2, 3)))
        release.check_windows_binary(binary, "1.2.3")
        with self.assertRaisesRegex(ValueError, "version does not match"):
            release.check_windows_binary(binary, "1.2.4")
        data = bytearray(binary.read_bytes())
        struct.pack_into("<H", data, 68, 0x14C)
        binary.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "x64"):
            release.check_windows_binary(binary, "1.2.3")

    def test_mac_architecture_type_and_deployment_target(self):
        binary = self.base / "FAHSaver"
        data = bytearray(56)
        struct.pack_into("<IIIIIIII", data, 0, 0xFEEDFACF, 0x0100000C, 0, 8, 1, 24, 0, 0)
        struct.pack_into("<IIIIII", data, 32, 0x32, 24, 1, 11 << 16, 0, 0)
        binary.write_bytes(data)
        release.check_macos_binary(binary)
        struct.pack_into("<I", data, 44, 12 << 16)
        binary.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "11.0"):
            release.check_macos_binary(binary)

    def test_zip_payload_hashes_and_symlink_rejection(self):
        stage = self.base / "stage"
        stage.mkdir()
        (stage / "FAHSaver.scr").write_bytes(b"executable")
        (stage / "FAHSaver.scr").chmod(0o755)
        destination = self.base / "payload.zip"
        release.write_zip(stage, destination, 1700000000)
        first = destination.read_bytes()
        release.write_zip(stage, destination, 1700000000)
        self.assertEqual(first, destination.read_bytes())
        with zipfile.ZipFile(destination) as archive:
            self.assertEqual(archive.namelist(), ["FAHSaver.scr"])
            self.assertEqual(archive.getinfo("FAHSaver.scr").external_attr >> 16 & 0o777, 0o755)
        (stage / "linked").symlink_to(stage / "FAHSaver.scr")
        with self.assertRaisesRegex(ValueError, "symbolic links"):
            release.write_zip(stage, destination, 1700000000)

    def test_release_checksum_inventory_and_tampering(self):
        version = "1.2.3"
        names = [f"FAHSaver-{platform}-v{version}.{suffix}"
                 for platform in ("macos-arm64", "windows-x64") for suffix in ("zip", "manifest.json")]
        names += [f"FAHSaver-Setup-{version}.exe", f"FAHSaver-Setup-{version}.manifest.json", f"FAHSaver-source-v{version}.tar.gz", "FAHSaver-macos-arm64.zip", "FAHSaver-Setup.exe"]
        for name in names:
            (self.base / name).write_text(name)
        (self.base / "FAHSaver-macos-arm64.zip").write_bytes((self.base / f"FAHSaver-macos-arm64-v{version}.zip").read_bytes())
        (self.base / "FAHSaver-Setup.exe").write_bytes((self.base / f"FAHSaver-Setup-{version}.exe").read_bytes())
        sums = "".join(f"{release.sha256(self.base / name)}  {name}\n" for name in names)
        (self.base / "SHA256SUMS").write_text(sums)
        self.assertEqual(len(release.verify(self.base, "v1.2.3")), 9)
        (self.base / names[0]).write_text("tampered")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            release.verify(self.base, "v1.2.3")

    def test_bootstrap_rejects_tampered_archive(self):
        archive = self.base / "zig.tar.xz"
        archive.write_bytes(b"wrong contents")
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            bootstrap.checked_archive(self.base, "zig", archive)

    def test_bootstrap_rejects_traversal_and_symlinks(self):
        specification = {"directory": "tool", "sha256": "a" * 64}
        for name, kind in (("tool/../../escape", tarfile.REGTYPE), ("tool/link", tarfile.SYMTYPE)):
            archive = self.base / "tool.tar"
            with tarfile.open(archive, "w") as target:
                member = tarfile.TarInfo(name)
                member.type = kind
                if kind == tarfile.SYMTYPE:
                    member.linkname = "/etc/passwd"
                target.addfile(member, io.BytesIO())
            with self.assertRaisesRegex(ValueError, "unsafe path or link"):
                bootstrap.extract_checked(archive, self.base, specification)
        self.assertFalse((self.base.parent / "escape").exists())

    def test_bootstrap_wrappers_are_relocatable(self):
        bootstrap.write_wrappers(self.base)
        for name in ("zig-cxx", "zig-ar", "zig-ranlib", "zig-rc"):
            wrapper = self.base / "bin" / name
            self.assertNotIn(str(self.base), wrapper.read_text())
            self.assertTrue(wrapper.stat().st_mode & 0o111)


if __name__ == "__main__":
    unittest.main()
