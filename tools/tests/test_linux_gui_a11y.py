import importlib.util
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("layerc_a11y_test", Path(__file__).resolve().parents[1] / "linux-gui/a11y.py")
a11y = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(a11y)

class Node:
    fetches = 0
    def __init__(self, name, children=(), showing=True):
        self.name, self.children, self.showing = name, list(children), showing
    @property
    def childCount(self):
        return len(self.children)
    def getChildAtIndex(self, index):
        Node.fetches += 1
        return self.children[index]
    def getState(self):
        return SimpleNamespace(contains=lambda state: state == "showing" and self.showing)
    def getRoleName(self):
        return "panel"
    def queryText(self):
        raise NotImplementedError
    def queryComponent(self):
        raise NotImplementedError

FAKE = SimpleNamespace(**{"STATE_" + name.upper(): name for name in
    ("focused", "selected", "enabled", "showing", "active", "modal", "checked")})

def flatten(item):
    if item is None:
        return []
    return [item] + [descendant for child in item.get("children", []) for descendant in flatten(child)]

class SnapshotBounds(unittest.TestCase):
    def setUp(self):
        Node.fetches = 0
        self.modules = patch.dict(sys.modules, {"pyatspi": FAKE})
        self.modules.start()
        self.addCleanup(self.modules.stop)
    def test_zero_budget_does_not_fetch(self):
        self.assertIsNone(a11y.describe(Node("root", [Node("child")]), 0, [0]))
        self.assertEqual(Node.fetches, 0)
    def test_node_budget_bounds_remote_child_fetches(self):
        root = Node("root", [Node(str(i)) for i in range(4310)])
        budget = [a11y.MAX_NODES]
        result = a11y.describe(root, 0, budget)
        self.assertEqual(len(flatten(result)), a11y.MAX_NODES)
        self.assertEqual(Node.fetches, a11y.MAX_NODES - 1)
        self.assertEqual(budget, [0])
        self.assertEqual(result["childCount"], 4310)
        self.assertEqual(result["childrenOmitted"], "node limit")
    def test_large_list_does_not_starve_sibling_controls(self):
        huge = Node("huge", [Node(str(i)) for i in range(4310)])
        root = Node("root", [huge, Node("controls", [Node("Save"), Node("Profile name")])])
        result = a11y.describe(root, 0, [20])
        names = {entry["name"] for entry in flatten(result)}
        self.assertTrue({"Save", "Profile name"} <= names)
        self.assertLessEqual(Node.fetches, 19)
    def test_hidden_subtree_is_explicitly_omitted(self):
        result = a11y.describe(Node("root", [Node("hidden", [Node("secret")], False), Node("visible")]), 0, [20])
        names = {entry["name"] for entry in flatten(result)}
        self.assertEqual(names, {"root", "hidden", "visible"})
        self.assertEqual(result["children"][0]["childrenOmitted"], "not showing")
        self.assertEqual(Node.fetches, 2)
    def test_depth_limit_does_not_fetch_descendants(self):
        result = a11y.describe(Node("root", [Node("child")]), a11y.MAX_DEPTH, [20])
        self.assertEqual(Node.fetches, 0)
        self.assertEqual(result["childrenOmitted"], "depth limit")
    def test_missing_child_fails_closed(self):
        with self.assertRaisesRegex(ValueError, "child disappeared"):
            a11y.describe(Node("root", [None]), 0, [20])
    def test_metadata_and_complete_small_tree_are_retained(self):
        result = a11y.describe(Node("root", [Node("child")]), 0, [20])
        self.assertEqual([n["name"] for n in flatten(result)], ["root", "child"])
        self.assertTrue(result["showing"])
        self.assertFalse(result["selected"])
        self.assertEqual(result["role"], "panel")
        self.assertEqual(result["childCount"], 1)
        self.assertNotIn("childrenOmitted", result)
        self.assertEqual(result["children"][0]["children"], [])

if __name__ == "__main__":
    unittest.main()
