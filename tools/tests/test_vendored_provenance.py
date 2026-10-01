"""Offline check that vendored miniz matches its recorded upstream bytes."""

import hashlib
import re
import unittest
from pathlib import Path

MINIZ = Path(__file__).parents[2] / "third_party" / "miniz"
LOCAL_FILES = {"UPSTREAM.md", "miniz_export.h"}

def normalized_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()

def recorded_hashes() -> dict[str, str]:
    record = (MINIZ / "UPSTREAM.md").read_text(encoding="utf-8")
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

if __name__ == "__main__":
    unittest.main()
