import io
import json
import sys
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

sys.path.insert(0, str(Path(__file__).parents[1] / "linux-gui"))
import bridge  # noqa: E402

# xmodmap -pk prints the numeric forms regardless of the spelling of a keysym.
XMODMAP_SAMPLE = """\
There are 7 KeySyms per KeyCode; KeyCodes range from 8 to 255.

    KeyCode     Keysym (Keysym)
      8
      9         0xff1b (Escape)
     10         0x0031 (1)      0x0021 (exclam)
     38         0x0061 (a)      0x0041 (A)
     47         0x00e9 (eacute)
     52         0x07eb (Greek_lambda)
    248
    250         0x0000 (NoSymbol)
    254         unparsed
    255
"""


def fake_keysym_to_utf32(symbol: int) -> int:
    if 0x01000000 <= symbol <= 0x0110FFFF:
        return symbol & 0x00FFFFFF
    if symbol == 0x07EB:
        return ord("λ")
    return symbol if symbol <= 0xFF else 0


class KeysymPlanTests(unittest.TestCase):
    def test_legacy_and_unicode_syms_are_reused_and_missing_chars_are_bound_once(self) -> None:
        keymap = XMODMAP_SAMPLE.replace("    248\n", "    248     0x101f642 (U0001F642)\n")
        plan = bridge.plan_keysym_bindings(keymap, "A!éλ中🙂中\t\n文", fake_keysym_to_utf32)
        self.assertEqual(plan, [(255, "U4E2D"), (250, "U6587")])

    def test_keeps_one_spare_without_replacing_existing_keys(self) -> None:
        keymap = "8\n9 0xff1b (Escape)\n254 unparsed\n255\n"
        with self.assertRaisesRegex(bridge.Rejected, "leaving one spare"):
            bridge.plan_keysym_bindings(keymap, "中文", fake_keysym_to_utf32)


class FakeConnection:
    """Stands in for an accepted Unix socket in exchange() tests."""

    def __init__(self, payload: bytes) -> None:
        self.payload = payload
        self.sent = b""

    def getsockopt(self, *_args: Any) -> bytes:
        import struct
        return struct.pack("3i", 1, bridge.os.geteuid() if hasattr(bridge.os, "geteuid") else 0, 1)

    def settimeout(self, _seconds: float) -> None:
        pass

    def recv(self, _size: int) -> bytes:
        chunk, self.payload = self.payload, b""
        return chunk

    def sendall(self, data: bytes) -> None:
        self.sent += data


class BridgeLogicTests(unittest.TestCase):
    def setUp(self) -> None:
        self.commands: list[list[str]] = []
        self.keymaps: list[str] = []
        self.fail_on: str | None = None
        self.original_run = bridge.run

        def fake_run(command: list[str], timeout: float = 10.0, check: bool = True) -> str:
            self.commands.append(command)
            if self.fail_on and self.fail_on in command:
                raise RuntimeError("injected failure")
            if command == ["xmodmap", "-pk"]:
                return self.keymaps.pop(0)
            return ""

        bridge.run = fake_run
        self.addCleanup(setattr, bridge, "run", self.original_run)
        self.addCleanup(setattr, bridge, "SETTLE_SECONDS", bridge.SETTLE_SECONDS)
        bridge.SETTLE_SECONDS = 0
        instance = object.__new__(bridge.Bridge)
        instance.width, instance.height = 1600, 1000
        instance.sequence, instance.shot_index = 0, 1
        instance.latest = {"id": 1, "sequence": 0}
        instance.keysym_checked = {chr(code) for code in range(32, 127)}
        instance.token = "a" * 64
        instance.actions = io.StringIO()
        self.confirmations = 0

        def confirm() -> None:
            self.confirmations += 1

        instance.confirm_display = confirm
        instance.deadline = float("inf")
        self.bridge = instance

    def test_leading_dash_text_stays_literal(self) -> None:
        self.bridge.handle({"op": "type", "screenshot": 1, "text": "--file=/etc/passwd"})
        typed = self.commands[-1]
        self.assertEqual(typed[:2], ["xdotool", "type"])
        self.assertEqual(typed[-2:], ["--", "--file=/etc/passwd"])

    def test_typing_verifies_persistent_key_before_emitting_input_and_caches_it(self) -> None:
        self.keymaps = [XMODMAP_SAMPLE, XMODMAP_SAMPLE.replace(
            "    255\n", "    255     0x1004e2d (U4E2D)    0x1004e2d (U4E2D)\n")]
        with mock.patch.object(bridge, "load_keysym_decoder", return_value=fake_keysym_to_utf32):
            self.bridge.handle({"op": "type", "screenshot": 1, "text": "中"})
            self.bridge.latest = {"id": 2, "sequence": 1}
            self.bridge.handle({"op": "type", "screenshot": 2, "text": "中"})
        self.assertEqual(self.commands[:3], [
            ["xmodmap", "-pk"], ["xmodmap", "-e", "keycode 255 = U4E2D U4E2D"],
            ["xmodmap", "-pk"]])
        self.assertEqual(len(self.commands), 5)
        self.assertTrue(all(cmd[:2] == ["xdotool", "type"] and cmd[-2:] == ["--", "中"]
                            for cmd in self.commands[3:]))
        self.assertGreaterEqual(self.confirmations, len(self.commands) + 2)

    def test_failed_keymap_verification_never_types_or_caches_the_character(self) -> None:
        self.keymaps = [XMODMAP_SAMPLE, XMODMAP_SAMPLE]
        with (
            mock.patch.object(bridge, "load_keysym_decoder", return_value=fake_keysym_to_utf32),
            self.assertRaisesRegex(bridge.Rejected, "did not retain the text key bindings"),
        ):
            self.bridge.handle({"op": "type", "screenshot": 1, "text": "中"})
        self.assertEqual(self.bridge.sequence, 1)
        self.assertNotIn("中", self.bridge.keysym_checked)
        self.assertEqual([cmd[0] for cmd in self.commands], ["xmodmap"] * 3)

    def test_partial_failure_still_consumes_freshness(self) -> None:
        self.fail_on = "type"
        with self.assertRaises(RuntimeError):
            self.bridge.handle({"op": "set_text", "screenshot": 1, "x": 10, "y": 10, "text": "x"})
        self.assertEqual(self.bridge.sequence, 1)
        with self.assertRaisesRegex(bridge.Rejected, "predates"):
            self.bridge.handle({"op": "key", "screenshot": 1, "combo": "Return"})

    def test_invalid_arguments_do_not_consume_freshness(self) -> None:
        for request in ({"op": "click", "screenshot": 1, "x": 99999, "y": 1},
                        {"op": "click", "screenshot": 1, "x": True, "y": 1},
                        {"op": "type", "screenshot": 1, "text": "x" * (bridge.MAX_TEXT + 1)},
                        {"op": "key", "screenshot": 1, "combo": "ctrl+s; rm"}):
            with self.subTest(request=request):
                with self.assertRaises(bridge.Rejected):
                    self.bridge.handle(request)
        self.assertEqual(self.bridge.sequence, 0)
        self.assertEqual(self.commands, [])

    def test_every_drag_step_reconfirms_the_display(self) -> None:
        self.bridge.handle({"op": "drag", "screenshot": 1, "x": 1, "y": 1, "to_x": 20, "to_y": 20})
        # begin + mousedown + 12 moves + mouseup, then the post-action check.
        self.assertEqual(len(self.commands), 14)
        self.assertGreaterEqual(self.confirmations, 16)

    def test_failed_press_still_releases_the_button(self) -> None:
        self.fail_on = "mousedown"
        with self.assertRaises(RuntimeError):
            self.bridge.handle({"op": "drag", "screenshot": 1, "x": 1, "y": 1,
                                "to_x": 5, "to_y": 5})
        self.assertIn("mouseup", self.commands[-1])

    def test_unreleasable_button_ends_the_session(self) -> None:
        self.fail_on = "mouseup"
        with self.assertRaises(bridge.DisplayLost):
            self.bridge.handle({"op": "drag", "screenshot": 1, "x": 1, "y": 1,
                                "to_x": 5, "to_y": 5})

    def test_operation_deadline_bounds_every_step(self) -> None:
        original = bridge.OPERATION_SECONDS
        self.addCleanup(setattr, bridge, "OPERATION_SECONDS", original)
        bridge.OPERATION_SECONDS = -1
        with self.assertRaisesRegex(RuntimeError, "deadline"):
            self.bridge.handle({"op": "key", "screenshot": 1, "combo": "Return"})
        self.assertEqual(self.commands, [])

    def test_long_budget_is_only_for_read_only_editor_resources(self) -> None:
        observed: list[float] = []

        def observe(_request: object) -> None:
            observed.append(self.bridge.deadline - bridge.time.monotonic())

        self.bridge.op_editor_resources = observe
        self.bridge.op_state = observe
        before = self.bridge.sequence
        self.bridge.handle({"op": "editor_resources", "view": "json"})
        self.bridge.handle({"op": "state"})
        self.assertAlmostEqual(observed[0], bridge.RESOURCE_SECONDS, delta=0.1)
        self.assertAlmostEqual(observed[1], bridge.OPERATION_SECONDS, delta=0.1)
        self.assertEqual(self.bridge.sequence, before)
        self.assertEqual(self.commands, [])

    def test_stale_or_missing_screenshot_is_rejected(self) -> None:
        with self.assertRaisesRegex(bridge.Rejected, "latest screenshot"):
            self.bridge.handle({"op": "key", "combo": "Return"})
        self.bridge.handle({"op": "key", "screenshot": 1, "combo": "Return"})
        with self.assertRaisesRegex(bridge.Rejected, "predates"):
            self.bridge.handle({"op": "key", "screenshot": 1, "combo": "Return"})

    def test_inspect_reports_one_pixel_column_runs(self) -> None:
        import tempfile
        import types
        from unittest import mock
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.bridge.shots = Path(directory.name)
        (self.bridge.shots / "001-list.png").write_bytes(b"MOCK, never decoded")
        pixels = bytes.fromhex("ffffff" * 3 + "000080" * 2 + "000000" + "ffffff")
        decoded = types.SimpleNamespace(stdout=pixels)
        with mock.patch.object(bridge.subprocess, "run", return_value=decoded) as convert:
            result = self.bridge.op_inspect({"screenshot": 1, "region": [39, 120, 1, 7], "runs": True})
            plain = self.bridge.op_inspect({"screenshot": 1, "region": [39, 120, 1, 7]})
        self.assertEqual(result["runs"], [[120, 122, "ffffff"], [123, 124, "000080"],
                                          [125, 125, "000000"], [126, 126, "ffffff"]])
        self.assertIn("1x7+39+120", convert.call_args.args[0])
        self.assertNotIn("runs", plain)
        for request in ({"screenshot": 1, "region": [39, 120, 2, 7], "runs": True},
                        {"screenshot": 1, "region": [39, 120, 1, 7], "runs": 1}):
            with self.subTest(request=request), self.assertRaisesRegex(bridge.Rejected, "one-pixel-wide"):
                self.bridge.op_inspect(request)

    @unittest.skipUnless(hasattr(bridge.os, "geteuid"), "peer credentials are Linux-only")
    def test_malformed_requests_are_contained(self) -> None:
        for payload in (b"[1, 2]\n", b"not json\n",
                        b'{"op": "state", "token": "\\u00e9"}\n',
                        b'{"op": "state", "token": 5}\n'):
            with self.subTest(payload=payload):
                connection = FakeConnection(payload)
                self.assertIsNone(self.bridge.exchange(connection))
                self.assertFalse(json.loads(connection.sent)["ok"])
        self.assertIn('"unparsed": true', self.bridge.actions.getvalue())


if __name__ == "__main__":
    unittest.main()
