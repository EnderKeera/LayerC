"""Reference-capture ordering guard, without a display or production access."""
import importlib
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[1] / "linux-gui"))
VisualWalk = importlib.import_module("visual_walkthrough").VisualWalk

class TemplateCapture(VisualWalk):
    def __init__(self):
        self.events = []

    def visual_durable(self):
        return {}

    def calibrate(self, label):
        pass

    def visual_import(self, name, label):
        pass

    def find(self, role, name):
        return name

    def click(self, node, label):
        if node == "Capture reference":
            self.events.append(("capture",))
            raise AssertionError("capture reached")

    def visual_start(self, label):
        self.events.append(("start",))

    def visual_preview_drag(self, label, start, end, mask=False):
        self.events.append(("region", start, end))

class TemplateCaptureTests(unittest.TestCase):
    def test_reference_capture_does_not_inherit_the_pixel_crop(self):
        walk = TemplateCapture()
        with self.assertRaisesRegex(AssertionError, "capture reached"):
            walk.case_visual_template_calibration()
        self.assertEqual(walk.events,
                         [("start",), ("region", (0.02, 0.02), (0.95, 0.95)),
                          ("capture",)])

if __name__ == "__main__":
    unittest.main()
