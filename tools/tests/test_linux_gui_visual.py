"""Nested modal and preview-coordinate guards; no display, process or store access."""
import importlib
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[1] / "linux-gui"))
VisualWalk = importlib.import_module("visual_walkthrough").VisualWalk
_walkthrough = importlib.import_module("walkthrough")
Failure, Walk = _walkthrough.Failure, _walkthrough.Walk


class Dialogs(Walk):
    def __init__(self, nodes):
        self.tree = nodes

    def nodes(self):
        return self.tree


def dialog(name, active=False, focused=False, showing=True):
    return [{"depth": 1, "role": "dialog", "name": name, "active": active, "showing": showing},
            {"depth": 2, "role": "button", "name": name + " button", "focused": focused}]


class DialogSelectionTests(unittest.TestCase):
    def test_active_confirmation_wins_regardless_of_export_order(self):
        parent = dialog("Calibration")
        prompt = dialog("Exact confirmation", active=True)
        for nodes in (parent + prompt, prompt + parent):
            with self.subTest(nodes=nodes):
                selected = Dialogs(nodes).dialog()
                self.assertEqual(selected[0]["name"], "Exact confirmation")
                self.assertEqual(selected[1][0]["name"], "Exact confirmation button")

    def test_focused_child_identifies_the_prompt(self):
        selected = Dialogs(dialog("Calibration") + dialog("Prompt", focused=True)).dialog()
        self.assertEqual(selected[0]["name"], "Prompt")

    def test_single_dialog_and_no_visible_dialog(self):
        self.assertEqual(Dialogs(dialog("Only")).dialog()[0]["name"], "Only")
        self.assertIsNone(Dialogs(dialog("Hidden", showing=False)).dialog())
        self.assertIsNone(Dialogs([]).dialog())

    def test_ambiguous_stack_is_refused_not_guessed(self):
        for nodes in (dialog("A") + dialog("B"),
                      dialog("A", active=True) + dialog("B", active=True)):
            with self.subTest(nodes=nodes), self.assertRaisesRegex(Failure, "multiple dialogs"):
                Dialogs(nodes).dialog()


class Preview(VisualWalk):
    def __init__(self, width, height):
        self.geometry = {"width": width, "height": height}
        self.actions = []

    def find(self, role, name):
        return {"extents": [100, 100, 550, 260]}

    def window(self, name):
        return self.geometry

    def act(self, op, label, **args):
        self.actions.append((op, args))


class PreviewCoordinateTests(unittest.TestCase):
    def test_region_drag_uses_observed_client_aspect(self):
        for width, height, expected in (
                (1100, 650, {"x": 155, "y": 100, "to_x": 595, "to_y": 360}),
                (400, 300, {"x": 202, "y": 100, "to_x": 548, "to_y": 360})):
            with self.subTest(width=width, height=height):
                preview = Preview(width, height)
                preview.visual_preview_drag("region", (0, 0), (1, 1))
                self.assertEqual(preview.actions, [("drag", expected)])

    def test_imported_square_mask_has_its_own_aspect(self):
        preview = Preview(1100, 650)
        preview.visual_preview_drag("mask", (0, 0), (1, 1), mask=True)
        self.assertEqual(preview.actions, [("drag", {"x": 245, "y": 100,
                                                    "to_x": 505, "to_y": 360})])

    def test_empty_fixture_cannot_receive_input(self):
        preview = Preview(1100, 0)
        with self.assertRaisesRegex(AssertionError, "geometry"):
            preview.visual_preview_drag("region", (0, 0), (1, 1))
        self.assertEqual(preview.actions, [])

class CaptureFixture(VisualWalk):
    def __init__(self, modal=(20, 50, 1060, 720), moves=True):
        self.shot = {"screen": [1600, 1000]}
        self.geometry = {"id": 7, "x": 40, "y": 60, "width": 1100, "height": 650}
        self.modal, self.moves, self.actions = modal, moves, []

    def window(self, name):
        return self.geometry

    def dialog(self):
        return ({"extents": self.modal}, []) if self.modal else None

    def act(self, op, label, **args):
        self.actions.append((op, args))
        if self.moves:
            self.geometry.update({key: args[key] for key in ("x", "y", "width", "height")})

class CaptureFixtureTests(unittest.TestCase):
    def test_controlled_pattern_is_exposed_before_capture(self):
        fixture = CaptureFixture()
        fixture.visual_prepare_fixture("capture")
        self.assertEqual(fixture.actions, [("set_geometry", {
            "window": 7, "x": 1176, "y": 40, "width": 400, "height": 300})])

    def test_obscured_or_unmoved_fixture_is_refused(self):
        for fixture, reason in ((CaptureFixture(modal=(1100, 20, 500, 800)), "obscured"),
                                (CaptureFixture(moves=False), "restore"),
                                (CaptureFixture(modal=None), "dialog")):
            with self.subTest(reason=reason), self.assertRaisesRegex(AssertionError, reason):
                fixture.visual_prepare_fixture("capture")

    def test_insufficient_display_is_refused_without_input(self):
        fixture = CaptureFixture()
        fixture.shot["screen"] = [400, 300]
        with self.assertRaisesRegex(AssertionError, "display"):
            fixture.visual_prepare_fixture("capture")
        self.assertEqual(fixture.actions, [])

    def test_shared_fixture_uses_a_visual_module_descriptor(self):
        import ast

        source = (Path(__file__).parents[1] / "linux-editor-gui-tests.sh").read_text(encoding="utf-8")
        marker = "python3 - \"$profiles\" \"$GUI_MODE\" <<'PY'\n"
        program = source.split(marker, 1)[1].split("\nPY\n", 1)[0]
        updates = [node for node in ast.walk(ast.parse(program))
                   if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                   and isinstance(node.func.value, ast.Name)
                   and node.func.value.id == "shared" and node.func.attr == "update"]
        self.assertEqual(len(updates), 1)
        modules = next(keyword.value for keyword in updates[0].keywords if keyword.arg == "modules")
        self.assertEqual(ast.literal_eval(modules), [{"kind": "visual", "schemaVersion": 2}])
        enabled = next(keyword.value for keyword in updates[0].keywords if keyword.arg == "enabled")
        self.assertFalse(ast.literal_eval(enabled), "preservation fixture must not compete for the game")


class ProfileFixtureSelectionTests(unittest.TestCase):
    def index(self, names, expected=None, fixture="GUI fixture"):
        locate = importlib.import_module("a11y_check").profile_fixture_index
        return locate([name + "  —  lctest.exe" for name in names],
                      names if expected is None else expected, fixture)

    def test_authored_name_whitespace_is_preserved(self):
        self.assertEqual(self.index(["GUI fixture "], fixture="GUI fixture "), 0)

    def test_noncanonical_separator_is_not_silently_normalized(self):
        locate = importlib.import_module("a11y_check").profile_fixture_index
        with self.assertRaisesRegex(AssertionError, "profile rows"):
            locate(["GUI fixture — lctest.exe"], ["GUI fixture"], "GUI fixture")

    def test_fixture_is_found_in_either_profile_order(self):
        self.assertEqual(self.index(["Shared fixture", "GUI fixture"]), 1)
        self.assertEqual(self.index(["GUI fixture", "Shared fixture"]), 0)

    def test_single_profile_is_still_supported(self):
        self.assertEqual(self.index(["GUI fixture"]), 0)

    def test_missing_extra_or_duplicated_rows_are_refused(self):
        expected = ["GUI fixture", "Shared fixture"]
        for names in (["GUI fixture"], ["GUI fixture", "Shared fixture", "Extra"],
                      ["GUI fixture", "GUI fixture"]):
            with self.subTest(names=names), self.assertRaisesRegex(AssertionError, "profile rows"):
                self.index(names, expected)

    def test_ambiguous_or_absent_fixture_is_refused(self):
        for names in (["GUI fixture", "GUI fixture"], ["Shared fixture"]):
            with self.subTest(names=names), self.assertRaisesRegex(AssertionError, "fixture row"):
                self.index(names)


if __name__ == "__main__":
    unittest.main()
