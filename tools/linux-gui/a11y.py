#!/usr/bin/env python3
"""Bounded AT-SPI snapshot for the bridge's `tree` operation (one JSON object).

Runs out of process so an absent or wedged accessibility bus cannot stall the
bridge. Reports `available: false` with a reason instead of guessing.
"""
from collections import deque
import json
import os
import sys

MAX_NODES = 400
MAX_DEPTH = 8
MAX_TEXT = 400
# Multi-line views whose full text the walkthrough verifies.
LONG_TEXT = ("Advanced automation JSON", "Runtime diagnostics", "Keyboard shortcuts — LayerE")


def describe_one(node, budget):
    import pyatspi  # noqa: PLC0415 -- imported only after the bus check
    if budget[0] <= 0:
        return None
    budget[0] -= 1
    states = node.getState()
    item = {"role": node.getRoleName(), "name": node.name,
            "focused": states.contains(pyatspi.STATE_FOCUSED),
            "selected": states.contains(pyatspi.STATE_SELECTED),
            "enabled": states.contains(pyatspi.STATE_ENABLED),
            "showing": states.contains(pyatspi.STATE_SHOWING),
            "active": states.contains(pyatspi.STATE_ACTIVE),
            "modal": states.contains(pyatspi.STATE_MODAL)}
    if states.contains(pyatspi.STATE_CHECKED):
        item["checked"] = True
    try:
        text = node.queryText()
        limit = 8192 if item["name"] in LONG_TEXT else MAX_TEXT
        item["text"] = text.getText(0, min(text.characterCount, limit))
        item["textTruncated"] = text.characterCount > limit
    except NotImplementedError:
        pass
    try:
        box = node.queryComponent().getExtents(pyatspi.DESKTOP_COORDS)
        item["extents"] = [box.x, box.y, box.width, box.height]
    except NotImplementedError:
        pass
    return item


def describe(node, depth, budget):
    """Bound remote work and fairly visit siblings without draining a huge list."""
    item = describe_one(node, budget)
    if item is None:
        return None
    pending = deque([(node, item, depth, 0, None)])
    while pending and budget[0] > 0:
        parent, entry, level, index, count = pending.popleft()
        if level >= MAX_DEPTH:
            entry["childrenOmitted"] = "depth limit"
            continue
        if not entry["showing"]:
            entry["children"] = []
            entry["childrenOmitted"] = "not showing"
            continue
        if count is None:
            count = parent.childCount
            entry["childCount"] = count
            entry["children"] = []
        if index >= count:
            continue
        child = parent.getChildAtIndex(index)
        if child is None:
            raise ValueError("accessibility child disappeared during snapshot")
        child_item = describe_one(child, budget)
        entry["children"].append(child_item)
        if index + 1 < count:
            pending.append((parent, entry, level, index + 1, count))
        pending.append((child, child_item, level + 1, 0, None))
    for _parent, entry, _level, index, count in pending:
        if count is None or index < count:
            entry["childrenOmitted"] = "node limit"
    return item


def main():
    if not os.environ.get("DBUS_SESSION_BUS_ADDRESS"):
        print(json.dumps({"available": False, "reason": "no private session bus"}))
        return 0
    try:
        import pyatspi  # noqa: PLC0415
    except ImportError as error:
        print(json.dumps({"available": False, "reason": f"pyatspi unavailable: {error}"}))
        return 0
    desktop = pyatspi.Registry.getDesktop(0)
    apps = [desktop.getChildAtIndex(i) for i in range(desktop.childCount)]
    names = [app.name for app in apps if app is not None]
    editor = next((app for app in apps if app is not None and "layere" in app.name.lower()), None)
    if editor is None:
        print(json.dumps({"available": False, "applications": names,
                          "reason": "LayerE exposes no accessibility tree on this bus"}))
        return 0
    tree = describe(editor, 0, [MAX_NODES])
    print(json.dumps({"available": True, "applications": names, "tree": tree}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
