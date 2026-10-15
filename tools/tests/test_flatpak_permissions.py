"""Keep the development backend's sandbox grants narrow and explicit."""
import json
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


class FlatpakPermissionsTests(unittest.TestCase):
    def test_exact_local_ipc_and_exchange_folder_permissions(self):
        manifest = json.loads((REPO / "packaging/flatpak/io.github.EnderKeera.LayerC.json")
                              .read_text(encoding="utf-8"))
        expected = {
            "--socket=x11", "--socket=wayland",
            "--talk-name=org.kde.KWin", "--talk-name=org.gnome.Shell",
            "--talk-name=io.github.EnderKeera.LayerC.GnomeShell",
            "--filesystem=xdg-documents", "--filesystem=xdg-download",
        }
        self.assertEqual(expected, set(manifest["finish-args"]))
        self.assertEqual(len(expected), len(manifest["finish-args"]))
        self.assertEqual("layere", manifest["command"])
        self.assertEqual("io.github.EnderKeera.LayerC", manifest["app-id"])


if __name__ == "__main__":
    unittest.main()
