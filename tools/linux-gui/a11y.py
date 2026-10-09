#!/usr/bin/env python3
"""Bounded AT-SPI snapshot for the bridge's `tree` operation (one JSON object).

Runs out of process so an absent or wedged accessibility bus cannot stall the
bridge. Reports `available: false` with a reason instead of guessing.
"""
import json
import os
import sys

MAX_NODES = 400
MAX_DEPTH = 8
MAX_TEXT = 400


def describe(node, depth, budget):
    import pyatspi  # noqa: PLC0415 -- imported only after the bus check
    if budget[0] <= 0:
        return None
    budget[0] -= 1
    states = node.getState()
    item = {"role": node.getRoleName(), "name": node.name,
            "focused": states.contains(pyatspi.STATE_FOCUSED),
            "selected": states.contains(pyatspi.STATE_SELECTED),
            "enabled": states.contains(pyatspi.STATE_ENABLED),
            "showing": states.contains(pyatspi.STATE_SHOWING)}
    if states.contains(pyatspi.STATE_CHECKED):
        item["checked"] = True
    try:
        text = node.queryText()
        item["text"] = text.getText(0, min(text.characterCount, MAX_TEXT))
    except NotImplementedError:
        pass
    try:
        box = node.queryComponent().getExtents(pyatspi.DESKTOP_COORDS)
        item["extents"] = [box.x, box.y, box.width, box.height]
    except NotImplementedError:
        pass
    if depth < MAX_DEPTH:
        children = [describe(node.getChildAtIndex(i), depth + 1, budget)
                    for i in range(node.childCount)]
        item["children"] = [child for child in children if child is not None]
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
