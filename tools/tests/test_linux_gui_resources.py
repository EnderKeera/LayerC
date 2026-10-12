"""Owned-editor identity and combo-selection guards; no /proc, display or store access."""
import json
import sys
import tempfile
import time
import unittest
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).parents[1] / "linux-gui"))
from automation_walkthrough import AutomationWalk  # noqa: E402

IDENTITY = {"pid": 1234, "startTime": "4242", "nonce": "n0nce"}
SAMPLER = {"applicationPid": 1234, "applicationStartTime": "4242", "nonce": "n0nce",
           "passed": True, "executable": "/opt/build/layere"}


@unittest.skipUnless(sys.platform.startswith("linux"), "the probe reads Linux /proc clocks")
class EditorIdentityTests(unittest.TestCase):
    def setUp(self) -> None:
        global editor_resources
        import editor_resources  # noqa: PLC0415 -- Linux-only sample helpers
        self.directory = tempfile.TemporaryDirectory()
        self.evidence = Path(self.directory.name)

    def tearDown(self) -> None:
        self.directory.cleanup()

    def write(self, identity: Any, sampler: Any) -> None:
        (self.evidence / "editor-identity.json").write_text(json.dumps(identity))
        (self.evidence / "startup-resources.json").write_text(json.dumps(sampler))

    def test_matching_owned_launch_is_accepted(self) -> None:
        self.write(IDENTITY, SAMPLER)
        self.assertEqual(editor_resources.read_identity(self.evidence),
                         (1234, "4242", "n0nce", "/opt/build/layere"))

    def test_mismatched_or_malformed_identity_is_refused(self) -> None:
        cases: list[tuple[Any, Any]] = [
            ([IDENTITY], SAMPLER),
            (IDENTITY, [SAMPLER]),
            ({**IDENTITY, "pid": True}, SAMPLER),
            ({**IDENTITY, "pid": 1235}, SAMPLER),
            ({**IDENTITY, "startTime": "42a"}, SAMPLER),
            ({**IDENTITY, "nonce": "other"}, SAMPLER),
            ({**IDENTITY, "nonce": ""}, {**SAMPLER, "nonce": ""}),
            (IDENTITY, {**SAMPLER, "applicationStartTime": "4243"}),
            (IDENTITY, {**SAMPLER, "passed": 1}),
            (IDENTITY, {**SAMPLER, "executable": "/usr/bin/layerd"}),
            (IDENTITY, {**SAMPLER, "executable": 7}),
        ]
        for identity, sampler in cases:
            with self.subTest(identity=identity, sampler=sampler):
                self.write(identity, sampler)
                with self.assertRaises(RuntimeError):
                    editor_resources.read_identity(self.evidence)
        (self.evidence / "editor-identity.json").write_text("{")
        with self.assertRaises(RuntimeError):
            editor_resources.read_identity(self.evidence)

    def test_short_operation_budget_is_refused_before_sampling(self) -> None:
        self.write(IDENTITY, SAMPLER)
        with self.assertRaisesRegex(RuntimeError, "budget"):
            editor_resources.measure_editor(self.evidence, ":7", self.evidence,
                                            time.monotonic() + 30)


class Combo(AutomationWalk):
    """A pull-down whose selection moves only if `moves` is true."""

    def __init__(self, selected: int, moves: bool, duplicate: bool = False) -> None:
        self.selected, self.moves, self.keys = selected, moves, []
        self.duplicate = duplicate

    def nodes(self) -> list[dict[str, Any]]:
        combos = 2 if self.duplicate else 1
        tree: list[dict[str, Any]] = []
        for _ in range(combos):
            tree.append({"role": "combo box", "name": "Speed", "depth": 3})
            tree += [{"role": "menu item", "name": str(i), "depth": 5,
                      "selected": i == self.selected} for i in range(3)]
        return tree

    def click(self, node: dict[str, Any], label: str) -> None:
        pass

    def key(self, combo: str, label: str) -> None:
        self.keys.append(combo)
        if self.moves and combo in ("Down", "Up"):
            self.selected += 1 if combo == "Down" else -1


class ComboSelectionTests(unittest.TestCase):
    def test_moves_relative_to_the_exported_selection(self) -> None:
        combo = Combo(selected=2, moves=True)
        combo.automation_choice("Speed", 0, "speed")
        self.assertEqual(combo.keys, ["Up", "Up", "Return"])
        self.assertEqual(combo.selected, 0)

    def test_unchanged_or_ambiguous_selection_fails(self) -> None:
        with self.assertRaisesRegex(AssertionError, "did not select"):
            Combo(selected=0, moves=False).automation_choice("Speed", 2, "speed")
        with self.assertRaisesRegex(AssertionError, "ambiguous"):
            Combo(selected=0, moves=True, duplicate=True).automation_choice("Speed", 1, "speed")
        with self.assertRaisesRegex(AssertionError, "no single selection"):
            Combo(selected=0, moves=True).automation_choice("Speed", 3, "speed")


if __name__ == "__main__":
    unittest.main()
