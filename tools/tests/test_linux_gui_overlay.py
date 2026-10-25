import io
import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[1] / "linux-gui"))
import bridge  # noqa: E402
import walkthrough  # noqa: E402
from overlay_walkthrough import EXPECTED as OVERLAY_EXPECTED  # noqa: E402
from overlay_walkthrough import OverlayWalk, expected_rect, lround  # noqa: E402

XEV_OUTPUT = """\
ButtonPress event, serial 18, synthetic NO, window 0x2200001,
    root 0x3d3, subw 0x0, time 1234, (645,488), root:(705,568),
    state 0x0, button 1, same_screen YES

ButtonRelease event, serial 18, synthetic NO, window 0x2200001,
    root 0x3d3, subw 0x0, time 1300, (645,488), root:(705,568),
    state 0x100, button 1, same_screen YES
"""


def window_info(x, y, width, height, override=False, viewable=True):
    return (f"  Absolute upper-left X:  {x}\n  Absolute upper-left Y:  {y}\n"
            f"  Width: {width}\n  Height: {height}\n"
            f"  Map State: {'IsViewable' if viewable else 'IsUnMapped'}\n"
            f"  Override Redirect State: {'yes' if override else 'no'}\n")


class FakeDisplay:
    """Canned X client output: LayerE 0x1e00003 and the fixture 0x2200001 are
    named; Openbox owns 0x800001; LayerD (0x2600000) owns three unnamed
    windows, one of them not override-redirect."""

    def __init__(self):
        self.info = {
            "31457283": window_info(20, 20, 900, 650),
            "35651585": window_info(60, 80, 1100, 650),
            "0x1e00009": window_info(30, 30, 120, 200, override=True),  # A LayerE menu.
            "0x800010": window_info(0, 0, 200, 300, override=True),     # An Openbox menu.
            "0x2600001": window_info(705, 553, 360, 30, override=True),
            "0x2600002": window_info(0, 0, 360, 30, override=True, viewable=False),
            "0x2600003": window_info(0, 0, 10, 10),
        }

    def run(self, command, timeout=10.0, check=True):
        if command[:2] == ["xdotool", "search"]:
            return "31457283\n35651585\n"
        if command[:2] == ["xdotool", "getwindowname"]:
            return {"31457283": "LayerE", "35651585": "LayerC Automation Fixture"}[command[2]]
        if command[:2] == ["xdotool", "getwindowpid"]:
            return "42"
        if command[0] == "xprop" and "_NET_FRAME_EXTENTS" in command:
            return "_NET_FRAME_EXTENTS(CARDINAL) = 1, 1, 22, 1"
        if command[0] == "xprop":
            return "_NET_SUPPORTING_WM_CHECK(WINDOW): window id # 0x800001"
        if command == ["xwininfo", "-root", "-children"]:
            return ("  6 children:\n"
                    "     0x1e00009 (has no name): ()  120x200+30+30  +30+30\n"
                    "     0x800010 (has no name): ()  200x300+0+0  +0+0\n"
                    "     0x2600001 (has no name): ()  360x30+705+553  +705+553\n"
                    "     0x2600002 (has no name): ()  360x30+0+0  +0+0\n"
                    "     0x2600003 (has no name): ()  10x10+0+0  +0+0\n"
                    "     0x800020 \"Openbox\": ()  1x1+-1+-1  +-1+-1\n")
        if command[:2] == ["xwininfo", "-id"]:
            return self.info[command[2]]
        raise AssertionError(f"unexpected command {command}")


class OverlayBridgeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.display = FakeDisplay()
        self.addCleanup(setattr, bridge, "run", bridge.run)
        bridge.run = self.display.run
        instance = object.__new__(bridge.Bridge)
        instance.width, instance.height = 1600, 1000
        instance.sequence, instance.shot_index = 0, 1
        instance.latest = {"id": 1, "sequence": 0}
        instance.actions = io.StringIO()
        instance.confirm_display = lambda: None
        instance.deadline = float("inf")
        self.bridge = instance

    def test_xev_button_events_are_parsed_in_order(self) -> None:
        events = bridge.parse_xev(XEV_OUTPUT.splitlines(keepends=True))
        self.assertEqual([e["type"] for e in events], ["ButtonPress", "ButtonRelease"])
        self.assertEqual(events[0], {"type": "ButtonPress", "synthetic": False, "window": 0x2200001,
                                     "x": 645, "y": 488, "rootX": 705, "rootY": 568, "button": 1})

    def test_synthetic_and_unrelated_xev_lines(self) -> None:
        text = XEV_OUTPUT.replace("synthetic NO", "synthetic YES", 1)
        events = bridge.parse_xev(["MotionNotify event, serial 1, synthetic NO, window 0x1,\n",
                                   *text.splitlines(keepends=True)])
        self.assertTrue(events[0]["synthetic"])
        self.assertEqual(len(events), 2)
        self.assertEqual(bridge.parse_xev([]), [])

    def test_overlays_are_daemon_override_redirect_windows_only(self) -> None:
        found = self.bridge.op_overlays({})
        self.assertEqual([w["id"] for w in found["mapped"]], [0x2600001])
        self.assertEqual([w["id"] for w in found["unmapped"]], [0x2600002])
        self.assertEqual(found["mapped"][0], {"id": 0x2600001, "client": 0x2600000,
                                              "x": 705, "y": 553, "width": 360, "height": 30})

    def test_click_through_is_a_fresh_input_action(self) -> None:
        self.assertIn("click_through", bridge.INPUT_OPS)
        with self.assertRaisesRegex(bridge.Rejected, "latest screenshot"):
            self.bridge.handle({"op": "click_through", "screenshot": 7, "x": 10, "y": 10,
                                "window": 35651585})
        with self.assertRaisesRegex(bridge.Rejected, "window"):
            self.bridge.handle({"op": "click_through", "screenshot": 1, "x": 10, "y": 10,
                                "window": 99})
        self.assertEqual(self.bridge.sequence, 0, "an invalid request consumed freshness")


class PipeObserverTests(unittest.TestCase):
    def pipe(self):
        reader, writer = os.pipe()
        self.addCleanup(os.close, reader)
        self.addCleanup(os.close, writer)
        return reader, writer

    def test_all_event_lines_are_read_without_buffer_loss(self) -> None:
        reader, writer = self.pipe()
        data = ("\n" + XEV_OUTPUT).encode("utf-8")
        self.assertEqual(os.write(writer, data), len(data))
        # Keep the writer open, like the running xev: no EOF makes a buffered
        # readline()/select() loop lose the lines buffered after the first one.
        observed = bridge.read_until(reader, b"ButtonRelease", 1.0)
        self.assertEqual(observed, data)
        events = bridge.parse_xev(observed.decode().splitlines(keepends=True))
        self.assertEqual([e["type"] for e in events], ["ButtonPress", "ButtonRelease"])

    def test_eof_before_marker_returns_partial_observation(self) -> None:
        reader, writer = os.pipe()
        self.addCleanup(os.close, reader)
        os.write(writer, b"partial observation\n")
        os.close(writer)
        self.assertEqual(bridge.read_until(reader, b"ButtonRelease", 1.0),
                         b"partial observation\n")

    def test_expired_deadline_does_not_wait_on_an_empty_pipe(self) -> None:
        reader, _ = self.pipe()
        self.assertEqual(bridge.read_until(reader, b"ButtonRelease", 0.0), b"")


class IsolationWalk(OverlayWalk):
    """Two equal names, distinct IDs, an imported copy and a failed name draft."""
    def __init__(self, copied=True, enabled=True):
        self.rows = {"fixture.json": {"json": {"id": "fixture", "name": "Fixture", "enabled": enabled,
                                               "opaque": {"kept": 7}}},
                     "disabled.json": {"json": {"id": "disabled", "name": "Disabled", "enabled": False}},
                     "unreadable.json": {"json": None},
                     "copied.json.bak": {"json": {"id": "copied", "name": "Fixture", "enabled": True}}}
        if copied:
            self.rows["copied.json"] = {"json": {"id": "copied", "name": "Fixture", "enabled": True}}
        self.current, self.pending = "fixture.json", None
        self.draft_name, self.draft_enabled = "Unsaved", enabled
        self.events = []

    def store(self):
        return self.rows

    def profile(self, filename="fixture.json"):
        return self.rows[filename], "unused-hash"

    def focus(self, *args):
        pass

    def key(self, *args):
        pass

    def profile_rows(self, name):
        return [f for f, e in self.rows.items()
                if f.endswith(".json") and e["json"] and e["json"]["name"] == name]

    def row(self, filename, label):
        if filename == self.current:
            return
        if self.draft_name != self.rows[self.current]["json"]["name"]:
            self.pending = filename
        else:
            self.current = filename
            self.draft_name = self.rows[filename]["json"]["name"]
            self.draft_enabled = self.rows[filename]["json"]["enabled"]

    def dialog(self):
        return self.pending is not None

    def press(self, button, label):
        assert button == "Discard" and self.pending is not None
        self.events.append("discard")
        self.current = self.pending
        self.pending = None
        self.draft_name = self.rows[self.current]["json"]["name"]
        self.draft_enabled = self.rows[self.current]["json"]["enabled"]

    def data(self, filename="fixture.json"):
        result = self.rows[filename]["json"]
        assert isinstance(result, dict)
        return result

    def text(self, name, role="text"):
        if name == "Profile ID (read-only)":
            assert role == "text"
            return self.data(self.current)["id"]
        assert name == "Profile name" and role == "entry"
        return self.draft_name

    def find(self, *args):
        return "Profile enabled"

    def click(self, widget, label):
        assert widget == "Profile enabled"
        self.draft_enabled = not self.draft_enabled

    def type_into(self, role, name, value, label):
        assert name == "Profile name"
        self.draft_name = value

    def save(self, label):
        self.rows[self.current]["json"].update(name=self.draft_name, enabled=self.draft_enabled)
        self.events.append("save:" + self.current)


class OverlayIsolationTests(unittest.TestCase):
    def test_imported_copy_is_disabled_by_id_and_failed_draft_discarded(self):
        walk = IsolationWalk()
        walk.isolate_overlay_fixture()
        self.assertFalse(walk.data("copied.json")["enabled"])
        self.assertTrue(walk.data("copied.json.bak")["enabled"])
        self.assertEqual(walk.current, "fixture.json")
        self.assertIn("discard", walk.events)
        self.assertIn("save:copied.json", walk.events)
        self.assertEqual(walk.rows["fixture.json"]["json"],
                         {"id": "fixture", "name": "Fixture", "enabled": True, "opaque": {"kept": 7}})
        self.assertIsNone(walk.rows["unreadable.json"]["json"])

    def test_no_copy_still_restores_a_failed_name_draft(self):
        walk = IsolationWalk(copied=False)
        walk.isolate_overlay_fixture()
        self.assertEqual(walk.draft_name, "Fixture")
        self.assertTrue(walk.data()["enabled"])

    def test_disabled_fixture_is_enabled_through_ui_and_save(self):
        walk = IsolationWalk(copied=False, enabled=False)
        walk.isolate_overlay_fixture()
        self.assertTrue(walk.data()["enabled"])
        self.assertIn("save:fixture.json", walk.events)


class OverlayManifestTests(unittest.TestCase):
    def test_overlay_cases_are_required_and_last(self) -> None:
        names = list(walkthrough.EXPECTED)
        self.assertEqual(names[-len(OVERLAY_EXPECTED):], list(OVERLAY_EXPECTED))
        self.assertEqual(set(OVERLAY_EXPECTED), {"overlay-focus-changes", "overlay-target-resize",
                                                 "overlay-click-through", "overlay-cleanup"})
        self.assertNotIn("overlay-target-changes", walkthrough.DEFERRED)
        for name in OVERLAY_EXPECTED:
            self.assertTrue(callable(getattr(walkthrough.Walk, "case_" + name.replace("-", "_"))))

    def test_expected_rect_matches_the_centred_anchor_rule(self) -> None:
        game = {"x": 60, "y": 80, "width": 1100, "height": 650}
        # lround(0.75 * 650) = lround(487.5) = 488, rounded half away from zero.
        self.assertEqual(expected_rect(game, {"anchorX": 0.75, "anchorY": 0.75}, (360, 30)),
                         (705, 553))
        self.assertEqual(expected_rect(game, {"anchorX": 0.25, "anchorY": 0.75}, (361, 31)),
                         (60 + 275 - 180, 80 + 488 - 15))
        self.assertEqual(lround(2.5), 3)


if __name__ == "__main__":
    unittest.main()
