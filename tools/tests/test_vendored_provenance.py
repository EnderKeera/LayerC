"""Offline checks of the recorded, unmodified vendored source snapshots."""

import hashlib
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).parents[2]
MINIZ = ROOT / "third_party" / "miniz"
FLTK = ROOT / "third_party" / "fltk"
LOCAL_FILES = {"UPSTREAM.md", "miniz_export.h"}

def normalized_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()

def recorded_hashes(directory: Path = MINIZ) -> dict[str, str]:
    record = (directory / "UPSTREAM.md").read_text(encoding="utf-8")
    return {
        name: digest
        for digest, name in re.findall(r"^ {4}([0-9a-f]{64})  (\S+)$", record, re.M)
    }

class VendoredProvenanceTests(unittest.TestCase):
    def test_upstream_files_match_recorded_hashes(self) -> None:
        recorded = recorded_hashes()
        self.assertIn("LICENSE", recorded)
        for name, digest in sorted(recorded.items()):
            with self.subTest(name):
                self.assertEqual(digest, normalized_sha256(MINIZ / name))

    def test_every_vendored_file_is_upstream_or_declared_local(self) -> None:
        present = {path.name for path in MINIZ.iterdir() if path.is_file()}
        recorded = set(recorded_hashes())
        self.assertEqual(set(), present - recorded - LOCAL_FILES, "unrecorded files")
        self.assertEqual(set(), recorded - present, "recorded files are missing")
        self.assertEqual(set(), recorded & LOCAL_FILES)

    def test_hashing_ignores_checkout_line_endings(self) -> None:
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            lf = Path(directory) / "lf"
            crlf = Path(directory) / "crlf"
            lf.write_bytes(b"a\nb\n")
            crlf.write_bytes(b"a\r\nb\r\n")
            self.assertEqual(normalized_sha256(lf), normalized_sha256(crlf))

class FltkProvenanceTests(unittest.TestCase):
    def test_upstream_files_match_recorded_hashes(self) -> None:
        recorded = recorded_hashes(FLTK)
        self.assertIn("COPYING", recorded)
        self.assertIn("FL/Fl.H", recorded)
        self.assertIn("CMakeLists.txt", recorded)
        for name, digest in sorted(recorded.items()):
            with self.subTest(name=name):
                self.assertEqual(digest, normalized_sha256(FLTK / name))

    def test_every_vendored_file_is_recorded(self) -> None:
        present = {path.relative_to(FLTK).as_posix()
                   for path in FLTK.rglob("*") if path.is_file()}
        recorded = set(recorded_hashes(FLTK))
        local = {"UPSTREAM.md"}
        self.assertEqual(set(), present - recorded - local, "unrecorded files")
        self.assertEqual(set(), recorded - present, "recorded files are missing")
        self.assertEqual(set(), recorded & local)

    def test_flatpak_uses_the_same_pinned_release_archive(self) -> None:
        manifest = json.loads((ROOT / "packaging/flatpak/io.github.EnderKeera.LayerC.json")
                              .read_text(encoding="utf-8"))
        modules = [module for module in manifest["modules"] if module["name"] == "fltk"]
        self.assertEqual(1, len(modules))
        self.assertEqual(1, len(modules[0]["sources"]))
        source = modules[0]["sources"][0]
        record = (FLTK / "UPSTREAM.md").read_text(encoding="utf-8")
        self.assertIn("Tag: release-1.4.5\n", record)
        self.assertIn(f'Archive: {source["url"]}\n', record)
        self.assertIn(f'Archive SHA-256: {source["sha256"]}\n', record)


if __name__ == "__main__":
    unittest.main()
