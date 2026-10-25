"""Target selection regressions; no display, process or store access."""
import sys
import unittest
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).parents[1] / "linux-gui"))
from walkthrough import Failure, Walk  # noqa: E402
from parity_walkthrough import reviewed_destinations  # noqa: E402



class ProfileRows(Walk):
    def __init__(self, labels: list[str]) -> None:
        self.tree = [{"role": "list", "name": "Profiles", "depth": 2}]
        self.tree += [{"role": "list item", "name": label, "depth": 3}
                      for label in labels]

    def nodes(self, tree: object = None) -> list[dict[str, Any]]:
        return self.tree

    def find(self, role: str, name: str, nodes: object = None) -> dict[str, Any]:
        return self.tree[0]


class FixtureRows(ProfileRows):
    def __init__(self, ids: list[str]) -> None:
        super().__init__(["Fixture  —  game"] * len(ids))
        self.ids, self.visited = ids, []
        self.current = -1

    def profile(self, name: str = "fixture.json") -> tuple[dict[str, Any], None]:
        return {"json": {"name": "Fixture", "id": "original"}}, None

    def row(self, index: int, label: str) -> None:
        self.current = index
        self.visited.append(index)

    def text(self, name: str, role: str = "text") -> str:
        if name != "Profile ID (read-only)":
            raise AssertionError("selection must verify identity, not the display name")
        return self.ids[self.current]


class WalkthroughSelectionTests(unittest.TestCase):
    def test_copied_name_prefix_does_not_select_the_copy(self) -> None:
        rows = ProfileRows(["Fixture (copy)  —  game", "Fixture  —  game"])
        self.assertEqual(rows.row_of("Fixture"), 1)
        self.assertEqual(rows.row_of("Fixture (copy)"), 0)

    def test_disabled_and_undecorated_names_are_exact(self) -> None:
        self.assertEqual(ProfileRows(["Other", "Fixture  (off)"]).row_of("Fixture"), 1)
        self.assertEqual(ProfileRows(["Fixture"]).row_of("Fixture"), 0)
        with self.assertRaises(Failure):
            ProfileRows(["Fixture copy", "Fixture (copy)  —  game"]).row_of("Fixture")

    def test_ambiguous_labels_fail_instead_of_guessing(self) -> None:
        with self.assertRaises(Failure):
            ProfileRows(["Fixture  —  first", "Fixture  —  second"]).row_of("Fixture")

    def test_other_lists_are_not_profile_candidates(self) -> None:
        rows = ProfileRows(["Fixture"])
        rows.tree += [{"role": "list", "name": "Other", "depth": 2},
                      {"role": "list item", "name": "Fixture", "depth": 3}]
        self.assertEqual(rows.row_of("Fixture"), 0)

    def test_same_name_copy_requires_the_original_id(self) -> None:
        rows = FixtureRows(["imported-copy", "original"])
        rows.select_fixture("select")
        self.assertEqual(rows.visited, [0, 1])
        with self.assertRaises(Failure):
            FixtureRows(["imported-copy"]).select_fixture("missing")

class ExportSummaryTests(unittest.TestCase):
    def test_message_box_uses_label_name_without_a_text_interface(self) -> None:
        dialog = ({"role": "dialog"}, [
            {"role": "label", "name": "?"},
            {"role": "label", "name": "Reviewed destinations:\nfirst.json — copy\nlast.json — copy\n"},
            {"role": "button", "name": "Cancel"},
        ])
        self.assertEqual(reviewed_destinations(dialog),
                         ["first.json — copy", "last.json — copy"])

    def test_no_summary_cannot_be_mistaken_for_two_profiles(self) -> None:
        self.assertEqual(reviewed_destinations(None), [])
        self.assertEqual(reviewed_destinations(({}, [{"role": "label", "name": "Existing profile collision."} ])), [])


if __name__ == "__main__":
    unittest.main()
