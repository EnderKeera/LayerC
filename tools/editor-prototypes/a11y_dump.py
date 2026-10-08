#!/usr/bin/env python3
"""Dumps the AT-SPI accessibility tree of applications on the session bus.

Used by the Linux editor toolkit comparison to check that standard controls
expose roles and names. Prints JSON: per application, the node count, named
node count, and the role/name of every node (depth-limited).
"""
import json
import sys

import pyatspi  # python3-pyatspi


def walk(node, depth, out):
    if node is None or depth > 25:
        return
    try:
        role = node.getRoleName()
        name = node.name or ""
    except Exception:  # A node can vanish while walking.
        return
    out.append({"depth": depth, "role": role, "name": name})
    for index in range(node.childCount):
        try:
            child = node.getChildAtIndex(index)
        except Exception:
            continue
        walk(child, depth + 1, out)


def main() -> int:
    wanted = sys.argv[1] if len(sys.argv) > 1 else ""
    desktop = pyatspi.Registry.getDesktop(0)
    apps = []
    for index in range(desktop.childCount):
        app = desktop.getChildAtIndex(index)
        if app is None or (wanted and wanted not in (app.name or "")):
            continue
        nodes = []
        walk(app, 0, nodes)
        apps.append({
            "application": app.name,
            "nodes": len(nodes),
            "namedNodes": sum(1 for node in nodes if node["name"]),
            "roles": sorted({node["role"] for node in nodes}),
            "tree": nodes,
        })
    json.dump({"applications": apps}, sys.stdout, indent=1)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
