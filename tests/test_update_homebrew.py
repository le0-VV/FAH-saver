# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline fixtures for the release-to-cask trust boundary."""

import copy
import hashlib
import importlib.util
import io
from pathlib import Path
import plistlib
import stat
import struct
import tempfile
import unittest
from unittest.mock import patch
import zipfile


SPEC = importlib.util.spec_from_file_location(
    "update_homebrew", Path(__file__).resolve().parents[1] / "tools/update-homebrew.py")
updater = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(updater)


def macho(cpu=0x0100000C, kind=8, platform=1, minimum=0x000B0000):
    command = struct.pack("<6I", 0x32, 24, platform, minimum, 0x001A0000, 0)
    return struct.pack("<8I", 0xFEEDFACF, cpu, 0, kind, 1, len(command), 0, 0) + command


def archive(version="1.0.0", executable=None, extra=None, info=None):
    metadata = {"CFBundleShortVersionString": version, "CFBundleExecutable": "FAHSaver",
                "CFBundleIdentifier": "org.fahsaver.screensaver", "CFBundlePackageType": "BNDL",
                "LSMinimumSystemVersion": "11.0"}
    metadata.update(info or {})
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as file:
        file.writestr("FAHSaver.saver/Contents/Info.plist", plistlib.dumps(metadata))
        item = zipfile.ZipInfo("FAHSaver.saver/Contents/MacOS/FAHSaver")
        item.external_attr = (stat.S_IFREG | 0o755) << 16
        file.writestr(item, macho() if executable is None else executable)
        if extra:
            file.writestr(*extra)
    return output.getvalue()


def release(data, tag="v1.0.0"):
    return {"tag_name": tag, "draft": False, "prerelease": False,
            "published_at": "2026-11-17T00:00:00Z", "assets": [{
                "name": f"FAHSaver-macos-arm64-{tag}.zip", "state": "uploaded",
                "size": len(data), "browser_download_url": updater.asset_url(tag),
                "digest": "sha256:" + hashlib.sha256(data).hexdigest()}]}


class ReleaseTests(unittest.TestCase):
    def test_strict_stable_tags_and_numeric_order(self):
        self.assertEqual(updater.parse_version("v1.10.0", "v"), (1, 10, 0))
        for tag in ("1.0.0", "v01.0.0", "v1.0", "v1.0.0-beta", "v1.0.0+build",
                    "v1.0.0\n", "v1.0.0/../../x", "v1.0.0$(id)", "v１.0.0", None):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                updater.parse_version(tag, "v")

    def test_requires_published_stable_exact_asset(self):
        data = archive()
        original = release(data)
        self.assertEqual(updater.release_asset(original, "v1.0.0")["size"], len(data))
        invalid = []
        for change in ({"draft": True}, {"prerelease": True}, {"published_at": None},
                       {"tag_name": "v1.0.1"}, {"assets": []}):
            invalid.append(dict(original, **change))
        for change in ({"name": "FAHSaver.zip"}, {"state": "new"}, {"size": 0},
                       {"size": True}, {"browser_download_url": "https://example.org/other.zip"}):
            item = copy.deepcopy(original)
            item["assets"][0].update(change)
            invalid.append(item)
        duplicate = copy.deepcopy(original)
        duplicate["assets"].append(duplicate["assets"][0])
        invalid.append(duplicate)
        for item in invalid:
            with self.subTest(item=item), self.assertRaises(ValueError):
                updater.release_asset(item, "v1.0.0")

    def test_download_size_hash_and_public_request(self):
        data = archive()
        asset = release(data)["assets"][0]
        for content, digest, succeeds in ((data, asset["digest"], True),
                                          (data[:-1], asset["digest"], False),
                                          (data, "sha256:" + "0" * 64, False)):
            with patch.object(updater.urllib.request, "urlopen", return_value=io.BytesIO(content)) as open_url:
                if succeeds:
                    actual, checksum = updater.download_asset(asset)
                    self.assertEqual(actual, data)
                    self.assertEqual(checksum, hashlib.sha256(data).hexdigest())
                else:
                    with self.assertRaises(ValueError):
                        updater.download_asset(dict(asset, digest=digest))
                request = open_url.call_args.args[0]
                self.assertNotIn("Authorization", request.headers)


class ArchiveTests(unittest.TestCase):
    def test_valid_arm64_bundle_with_manifest(self):
        updater.validate_archive(archive(extra=("manifest.json", "{}")), "1.0.0")

    def test_rejects_wrong_version_identity_architecture_and_minimum(self):
        for data in (archive("1.0.1"), archive(info={"CFBundleExecutable": "Other"}),
                     archive(info={"CFBundleIdentifier": "org.other"}),
                     archive(info={"LSMinimumSystemVersion": "12.0"}),
                     archive(executable=macho(cpu=0x01000007)),
                     archive(executable=macho(kind=2)),
                     archive(executable=macho(platform=2)),
                     archive(executable=macho(minimum=0x000C0000)),
                     archive(executable=b"truncated"),
                     archive(executable=macho()[:-1])):
            with self.subTest(data=data[:20]), self.assertRaises(ValueError):
                updater.validate_archive(data, "1.0.0")

    def test_rejects_unsafe_and_duplicate_zip_members(self):
        link = zipfile.ZipInfo("FAHSaver.saver/Contents/link")
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        extras = [("../outside", "x"), ("/outside", "x"), ("folder\\outside", "x"),
                  ("folder/./outside", "x"), (link, "../../outside"),
                  ("FAHSaver.saver/Contents/info.plist", "case-insensitive duplicate")]
        for extra in extras:
            with self.subTest(extra=extra), self.assertRaises(ValueError):
                updater.validate_archive(archive(extra=extra), "1.0.0")
        with self.assertWarns(UserWarning):
            data = archive(extra=("FAHSaver.saver/Contents/Info.plist", "duplicate"))
        with self.assertRaises(ValueError):
            updater.validate_archive(data, "1.0.0")


class CaskTests(unittest.TestCase):
    def test_first_release_and_upgrade_preserve_caveats(self):
        initial = updater.updated_cask(None, "1.0.0", "a" * 64)
        self.assertIn('screen_saver "FAHSaver.saver"', initial)
        self.assertIn("depends_on arch: :arm64", initial)
        self.assertIn("depends_on :macos", initial)
        self.assertNotIn(":no_check", initial)
        changed = updater.updated_cask(initial, "1.10.0", "b" * 64)
        self.assertIn('version "1.10.0"', changed)
        self.assertIn('sha256 "' + "b" * 64 + '"', changed)
        self.assertEqual(changed[changed.index("  caveats"):], initial[initial.index("  caveats"):])
        self.assertEqual(updater.updated_cask(changed, "1.10.0", "b" * 64), changed)

    def test_refuses_downgrade_replaced_asset_and_unexpected_cask(self):
        initial = updater.new_cask("1.10.0", "a" * 64)
        for prior, version, digest in ((initial, "1.9.0", "b" * 64),
                                       (initial, "1.10.0", "b" * 64),
                                       (initial.replace(updater.CASK_URL, "https://example.org/x.zip"), "2.0.0", "b" * 64),
                                       (initial + '  version "1.10.0"\n', "2.0.0", "b" * 64),
                                       (None, "1.0.0", ":no_check")):
            with self.subTest(version=version), self.assertRaises(ValueError):
                updater.updated_cask(prior, version, digest)

    def test_dry_run_verifies_without_creating_then_writes_and_repeats(self):
        data = archive()
        asset = release(data)
        digest = hashlib.sha256(data).hexdigest()
        with tempfile.TemporaryDirectory() as folder:
            cask = Path(folder) / "Casks/fah-saver.rb"
            args = ["--tag", "v1.0.0", "--cask", str(cask)]
            with patch.object(updater, "fetch_release", return_value=asset), \
                    patch.object(updater, "download_asset", return_value=(data, digest)), \
                    patch("sys.stdout", new_callable=io.StringIO) as output:
                self.assertEqual(updater.main(args + ["--dry-run"]), 0)
                self.assertFalse(cask.exists())
                self.assertIn(digest, output.getvalue())
                self.assertEqual(updater.main(args), 0)
                content = cask.read_text()
                self.assertEqual(content, updater.new_cask("1.0.0", digest))
                self.assertEqual(updater.main(args), 0)
                self.assertEqual(cask.read_text(), content)


if __name__ == "__main__":
    unittest.main()
