"""Semantic row targeting and bounded wheel actions; no real display/store access."""
import io
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[1] / "linux-gui"))
import bridge  # noqa: E402
from cases import EXPECTED, merge_cases  # noqa: E402
from walkthrough import Failure, Walk  # noqa: E402


class SemanticRows(Walk):
    def __init__(self, visible=(0, 1), target_after_scroll=2):
        self.visible = set(visible)
        self.target_after_scroll = target_after_scroll
        self.actions = []
        self.captures = []
        self.shot = {"screenshot": 1}
        self.extra_list = False
        self.selected = [True, False, False]

    def nodes(self):
        result = [{"role": "list", "name": "Profiles", "depth": 2,
                   "extents": [10, 20, 200, 100]}]
        result.extend({"role": "list item", "name": f"Profile {index}", "depth": 3,
                       "selected": self.selected[index], "showing": index in self.visible,
                       "extents": [12, 22 + index * 20, 180, 18]
                       if index in self.visible else [0, 0, 0, 0]}
                      for index in range(3))
        if self.extra_list:
            result.append({"role": "list", "name": "Profiles", "depth": 2,
                           "extents": [10, 20, 200, 100]})
        return result

    def capture(self, label):
        self.captures.append(label)
        self.shot = {"screenshot": self.shot["screenshot"] + 1}
        return self.shot

    def act(self, op, label, **args):
        self.actions.append((op, label, args, self.shot["screenshot"]))
        if op == "scroll" and self.target_after_scroll is not None:
            self.visible = {self.target_after_scroll}
        return self.capture(label)


class SemanticRowTests(unittest.TestCase):
    def test_showing_row_clicks_its_semantic_center_from_a_fresh_capture(self):
        walk = SemanticRows()
        walk.row(1, "select", button=3)
        self.assertEqual(walk.actions, [("click", "select",
                                        {"x": 102, "y": 51, "button": 3}, 2)])
        self.assertEqual(walk.captures, ["select-target", "select"])

    def test_hidden_row_scrolls_then_clicks_without_intermediate_selection(self):
        walk = SemanticRows()
        walk.row(2, "select")
        self.assertEqual([(op, args) for op, _label, args, _shot in walk.actions], [
            ("scroll", {"x": 110, "y": 70, "steps": 6}),
            ("click", {"x": 102, "y": 71, "button": 1}),
        ])
        self.assertEqual([action[3] for action in walk.actions], [2, 4])
        self.assertEqual(walk.selected, [True, False, False])

    def test_row_above_viewport_scrolls_up(self):
        walk = SemanticRows(visible=(1, 2), target_after_scroll=0)
        walk.row(0, "up")
        self.assertEqual(walk.actions[0][2]["steps"], -6)
        self.assertEqual(walk.actions[-1][0], "click")

    def test_missing_row_and_duplicate_list_fail_without_input(self):
        walk = SemanticRows()
        with self.assertRaisesRegex(Failure, "does not exist"):
            walk.row(3, "missing")
        walk.extra_list = True
        with self.assertRaisesRegex(Failure, "missing or ambiguous"):
            walk.row(0, "ambiguous")
        self.assertEqual(walk.actions, [])

    def test_ambiguous_selection_and_hidden_gap_fail_without_input(self):
        walk = SemanticRows()
        walk.selected = [True, True, False]
        with self.assertRaisesRegex(Failure, "more than one"):
            walk.row(0, "ambiguous")
        walk.selected = [False] * 3
        walk.visible = {0, 2}
        with self.assertRaisesRegex(Failure, "hidden inside"):
            walk.row(1, "gap")
        self.assertEqual(walk.actions, [])

    def test_scroll_is_bounded_and_never_clicks_a_hidden_row(self):
        walk = SemanticRows(target_after_scroll=None)
        with self.assertRaisesRegex(Failure, "after scrolling"):
            walk.row(2, "stuck")
        self.assertEqual(len(walk.actions), 20)
        self.assertTrue(all(action[0] == "scroll" for action in walk.actions))

    def test_registry_keeps_all_cases_and_rejects_duplicate_ids(self):
        self.assertEqual(len(EXPECTED), 39)
        self.assertEqual(next(iter(EXPECTED)), "bridge-guards")
        with self.assertRaisesRegex(ValueError, "duplicate required case IDs: a"):
            merge_cases({"a": "one"}, {"a": "two"})


class ScrollBridgeTests(unittest.TestCase):
    def setUp(self):
        instance = object.__new__(bridge.Bridge)
        instance.width, instance.height = 1600, 1000
        instance.sequence, instance.shot_index = 0, 1
        instance.latest = {"id": 1, "sequence": 0}
        instance.token = "a" * 64
        instance.actions = io.StringIO()
        instance.confirm_display = lambda: None
        instance.deadline = float("inf")
        self.bridge = instance
        self.commands = []
        self.patch_run = patch.object(bridge, "run", side_effect=self.run_command)
        self.patch_run.start()
        self.addCleanup(self.patch_run.stop)
        self.patch_settle = patch.object(bridge, "SETTLE_SECONDS", 0)
        self.patch_settle.start()
        self.addCleanup(self.patch_settle.stop)

    def run_command(self, command, timeout=10.0, check=True):
        self.commands.append(command)
        return ""

    def request(self, steps, **extra):
        request = {"op": "scroll", "screenshot": 1, "x": 10, "y": 20, "steps": steps}
        request.update(extra)
        return self.bridge.handle(request)

    def test_direction_and_repeat_are_real_bounded_wheel_input(self):
        self.request(-2)
        self.assertEqual(self.commands[-1], ["xdotool", "mousemove", "10", "20", "click",
                                             "--repeat", "2", "4"])
        self.bridge.latest = {"id": 2, "sequence": self.bridge.sequence}
        self.bridge.handle({"op": "scroll", "screenshot": 2, "x": 10, "y": 20, "steps": 3})
        self.assertEqual(self.commands[-1], ["xdotool", "mousemove", "10", "20", "click",
                                             "--repeat", "3", "5"])

    def test_invalid_arguments_never_consume_freshness(self):
        for steps in (0, True, -11, 11, "2"):
            with self.subTest(steps=steps):
                with self.assertRaises(bridge.Rejected):
                    self.request(steps)
        with self.assertRaises(bridge.Rejected):
            self.request(1, x=99999)
        self.assertEqual(self.bridge.sequence, 0)
        self.assertEqual(self.commands, [])

    def test_accepted_scroll_consumes_freshness(self):
        self.request(1)
        with self.assertRaisesRegex(bridge.Rejected, "predates"):
            self.request(1)


if __name__ == "__main__":
    unittest.main()
