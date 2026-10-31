"""Pure typed-item walkthrough recipe/selection tests; no display/store access."""
from copy import deepcopy
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / "linux-gui"))
from items_walkthrough import COUNT, FLAG, EXPECTED, ItemsWalk, ordered_rows, probe_graph  # noqa: E402
from automation_walkthrough import EXPECTED as AUTOMATION_EXPECTED  # noqa: E402
from visual_walkthrough import EXPECTED as VISUAL_EXPECTED  # noqa: E402
from cases import EXPECTED as ALL_EXPECTED  # noqa: E402
from walkthrough import Walk  # noqa: E402


class ItemProbeRecipeTests(unittest.TestCase):
    def original(self):
        return {
            "schemaVersion": 1, "profileId": "fixture",
            "variables": [{"id": "old", "type": "bool", "persistent": True, "default": False}],
            "automations": [{"id": "original", "name": "Original", "enabled": True, "rules": []}],
        }

    def test_probe_is_independent_and_keeps_original_declared_variables(self):
        original = self.original()
        frozen = deepcopy(original)
        result = probe_graph(original, "dash", "keyboard:e")
        self.assertEqual(original, frozen)
        self.assertEqual(result["schemaVersion"], 1)
        self.assertEqual(result["profileId"], "fixture")
        self.assertEqual(result["variables"][0], original["variables"][0])
        result["variables"][0]["default"] = True
        self.assertEqual(original, frozen)

    def test_probe_uses_parser_wire_types_and_exact_state_transition(self):
        result = probe_graph(self.original(), "dash", "keyboard:e")
        declared = {row["id"]: row for row in result["variables"]}
        self.assertEqual(declared[COUNT], {
            "id": COUNT, "type": "int", "persistent": False, "default": 3})
        self.assertEqual(declared[FLAG], {
            "id": FLAG, "type": "bool", "persistent": False, "default": True})
        self.assertEqual(len(result["automations"]), 1)
        rule = result["automations"][0]["rules"][0]
        self.assertEqual(rule["event"], "inputPressed")
        self.assertEqual(rule["source"], "keyboard:e")
        self.assertEqual(rule["condition"],
                         {"op": "literal", "valueType": "bool", "value": True})
        self.assertEqual(rule["actions"], [
            {"type": "setVariable", "target": COUNT,
             "value": {"op": "literal", "valueType": "int", "value": 7}},
            {"type": "setVariable", "target": FLAG,
             "value": {"op": "literal", "valueType": "bool", "value": False}},
            {"type": "triggerCooldown", "target": "dash"},
        ])

    def test_probe_refuses_collisions_instead_of_rebinding_existing_variables(self):
        for reserved in (COUNT, FLAG):
            with self.subTest(reserved=reserved):
                original = self.original()
                original["variables"].append({"id": reserved, "type": "int", "default": 0})
                frozen = deepcopy(original)
                with self.assertRaisesRegex(AssertionError, "already exist"):
                    probe_graph(original, "dash", "keyboard:e")
                self.assertEqual(original, frozen)


class OrderedItemSelectionTests(unittest.TestCase):
    def test_only_rows_in_the_named_list_are_returned(self):
        nodes = [
            {"role": "list", "name": "Profiles", "depth": 2},
            {"role": "list item", "name": "Not an item", "depth": 3},
            {"role": "list", "name": "Panel items (ordered)", "depth": 2},
            {"role": "list item", "name": "Cooldown", "depth": 3},
            {"role": "list item", "name": "@future literal", "depth": 3},
            {"role": "list", "name": "Automations", "depth": 2},
            {"role": "list item", "name": "Not an item either", "depth": 3},
        ]
        self.assertEqual([row["name"] for row in ordered_rows(nodes)],
                         ["Cooldown", "@future literal"])

    def test_missing_or_ambiguous_item_lists_fail_visibly(self):
        with self.assertRaisesRegex(AssertionError, "missing or ambiguous"):
            ordered_rows([])
        node = {"role": "list", "name": "Panel items (ordered)", "depth": 2}
        with self.assertRaisesRegex(AssertionError, "missing or ambiguous"):
            ordered_rows([node, dict(node)])


class ItemControlContractTests(unittest.TestCase):
    def test_source_selection_uses_the_native_em_dash_label(self):
        selected = []
        walk = SimpleNamespace(
            choice_items=lambda name: (None, [
                {"name": "[1] Other — other"}, {"name": "[2] Count — typed_items_count"}]),
            automation_choice=lambda *args: selected.append(args),
        )
        ItemsWalk.items_choose_source(walk, COUNT, "source")
        self.assertEqual(selected, [("Source", 1, "source")])
        walk.choice_items = lambda name: (None, [{"name": "[1] typed_items_count"}])
        selected.clear()
        ItemsWalk.items_choose_source(walk, COUNT, "bare")
        self.assertEqual(selected, [("Source", 0, "bare")])
        walk.choice_items = lambda name: (None, [
            {"name": "[1] Count — typed_items_count"}, {"name": "[2] Duplicate — typed_items_count"}])
        with self.assertRaisesRegex(AssertionError, "missing/ambiguous"):
            ItemsWalk.items_choose_source(walk, COUNT, "ambiguous")

    def test_semantic_kinds_select_the_real_six_choice_item_dialog(self):
        for kind, index in (("counter", 3), ("indicator", 4),
                            ("active_timer", 2), ("preset", 5)):
            with self.subTest(kind=kind):
                calls = []
                walk = SimpleNamespace(
                    find=lambda role, name: (role, name),
                    click=lambda *args: calls.append(("click", *args)),
                    automation_choice=lambda *args: calls.append(("choice", *args)),
                    items_choose_source=lambda *args: calls.append(("source", *args)),
                )
                ItemsWalk.items_open(walk, kind, "source-id", "add")
                self.assertEqual(calls, [
                    ("click", ("button", "Add item"), "add-open"),
                    ("choice", "Item type", index, "add-kind"),
                    ("source", "source-id", "add-source"),
                ])


class ItemManifestTests(unittest.TestCase):
    def test_cases_are_declared_once_in_their_required_execution_order(self):
        keys = list(ALL_EXPECTED)
        item_keys = list(EXPECTED)
        self.assertEqual(len(item_keys), 3)
        self.assertEqual([key for key in keys if key in EXPECTED], item_keys)
        self.assertLess(max(keys.index(key) for key in AUTOMATION_EXPECTED),
                        min(keys.index(key) for key in EXPECTED))
        self.assertLess(max(keys.index(key) for key in EXPECTED),
                        min(keys.index(key) for key in VISUAL_EXPECTED))

    def test_every_new_required_case_has_a_real_runner_method(self):
        for case_id in EXPECTED:
            with self.subTest(case_id=case_id):
                self.assertTrue(callable(getattr(Walk, "case_" + case_id.replace("-", "_"), None)))


if __name__ == "__main__":
    unittest.main()
