"""Installer input checks, including path quoting and a real NSIS compilation."""

import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("packager", ROOT / "tools/package-windows-installer.py")
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)


class PackagerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="fah setup $ paths ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.payload = self.root / "payload with spaces"
        self.payload.mkdir()
        self.names = json.loads((packager.INSTALLER / "payload-files.json").read_text())
        for name in self.names:
            path = self.payload / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"synthetic installer test payload\n")
        pe = bytearray(88)
        pe[:2] = b"MZ"
        struct.pack_into("<I", pe, 0x3C, 64)
        pe[64:68] = b"PE\0\0"
        struct.pack_into("<H", pe, 68, 0x8664)
        (self.payload / "FAHSaver.scr").write_bytes(pe)
        self.write_manifest()

    def write_manifest(self):
        files = {name: hashlib.sha256((self.payload / name).read_bytes()).hexdigest() for name in self.names}
        (self.payload / "manifest.json").write_text(json.dumps({"version": "0.1.10", "files": files}))

    def test_checked_payload(self):
        manifest, files = packager.validate_payload(self.payload)
        self.assertEqual(manifest["version"], "0.1.10")
        self.assertEqual(set(files), set(self.names))
        include = packager.payload_include(self.payload, files)
        self.assertIn(" $ paths ", include)
        self.assertIn('!insertmacro ${ACTION} "Uninstall.exe"', include)

    def test_tampered_and_missing_payload(self):
        path = self.payload / self.names[-1]
        path.write_text("tampered")
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            packager.validate_payload(self.payload)
        path.unlink()
        with self.assertRaisesRegex(ValueError, "Missing"):
            packager.validate_payload(self.payload)

    def test_linked_payload_and_non_x64_executable(self):
        path = self.payload / self.names[-1]
        other = self.root / "outside.txt"
        other.write_text("outside")
        path.unlink()
        path.symlink_to(other)
        with self.assertRaisesRegex(ValueError, "linked"):
            packager.validate_payload(self.payload)
        path.unlink()
        path.write_text("restored")
        pe = bytearray((self.payload / "FAHSaver.scr").read_bytes())
        struct.pack_into("<H", pe, 68, 0x14C)
        (self.payload / "FAHSaver.scr").write_bytes(pe)
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "x64"):
            packager.validate_payload(self.payload)

    def test_output_cannot_replace_payload(self):
        with self.assertRaisesRegex(ValueError, "outside"):
            packager.build(self.payload, self.payload / "fah-preview.exe", "missing compiler")
        with self.assertRaisesRegex(ValueError, "newline"):
            packager.nsis_escape("bad\npath")

    @unittest.skipUnless(shutil.which("makensis"), "NSIS is not installed")
    def test_real_compilation_with_spaced_dollar_paths(self):
        output = self.root / "FAH Saver Setup.exe"
        report = packager.build(self.payload, output, "makensis")
        self.assertEqual(packager.pe_machine(output), 0x14C)
        self.assertEqual(report["payload_files"].keys(), set(self.names))
        self.assertTrue(output.with_suffix(".manifest.json").is_file())


if __name__ == "__main__":
    unittest.main()
