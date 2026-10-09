import io
import json
import sys
import unittest
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).parents[1] / "linux-gui"))
import bridge  # noqa: E402


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
        self.fail_on: str | None = None
        self.original_run = bridge.run

        def fake_run(command: list[str], timeout: float = 10.0, check: bool = True) -> str:
            self.commands.append(command)
            if self.fail_on and self.fail_on in command:
                raise RuntimeError("injected failure")
            return ""

        bridge.run = fake_run
        self.addCleanup(setattr, bridge, "run", self.original_run)
        self.addCleanup(setattr, bridge, "SETTLE_SECONDS", bridge.SETTLE_SECONDS)
        bridge.SETTLE_SECONDS = 0
        instance = object.__new__(bridge.Bridge)
        instance.width, instance.height = 1600, 1000
        instance.sequence, instance.shot_index = 0, 1
        instance.latest = {"id": 1, "sequence": 0}
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

    def test_stale_or_missing_screenshot_is_rejected(self) -> None:
        with self.assertRaisesRegex(bridge.Rejected, "latest screenshot"):
            self.bridge.handle({"op": "key", "combo": "Return"})
        self.bridge.handle({"op": "key", "screenshot": 1, "combo": "Return"})
        with self.assertRaisesRegex(bridge.Rejected, "predates"):
            self.bridge.handle({"op": "key", "screenshot": 1, "combo": "Return"})

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
