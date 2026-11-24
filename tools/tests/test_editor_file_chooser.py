"""Keep native file dialogs behind the editor's capture privacy decision."""

import re
import unittest
from pathlib import Path


class EditorFileChooser(unittest.TestCase):
    def test_native_chooser_calls_are_owned(self):
        root = Path(__file__).resolve().parents[2]
        sites = {
            source.relative_to(root).as_posix()
            for source in (root / "src/editor").rglob("*")
            if source.suffix in {".cpp", ".hpp", ".h", ".cxx"}
            and re.search(r"\bFl_Native_File_Chooser\b", source.read_text(encoding="utf-8"))
        }
        self.assertEqual({
            "src/editor/fltk/editor_file_chooser.cpp",
            "src/editor/fltk/calibration_window.cpp",
        }, sites)


if __name__ == "__main__":
    unittest.main()
