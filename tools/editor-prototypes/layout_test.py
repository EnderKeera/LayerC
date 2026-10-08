#!/usr/bin/env python3
"""Window-manager resize/focus/HiDPI probe in a private prototype session."""
import argparse
import json
import subprocess
import time
import traceback
from pathlib import Path

import gi
import pyatspi

gi.require_version("Atspi", "2.0")
from gi.repository import GLib  # noqa: E402 -- GI version selected first

parser = argparse.ArgumentParser()
parser.add_argument("output", type=Path)
parser.add_argument("application")
parser.add_argument("window")
parser.add_argument("scale", type=float)
parser.add_argument("store", type=Path, help="disposable fixture store checked after Save")
args = parser.parse_args()
if args.scale not in (1, 2):
    raise SystemExit("only the recorded 1x/2x probe scales are supported")
args.output.mkdir(parents=True, exist_ok=True)
report = {"passed": False, "scale": args.scale, "checks": [], "actions": [], "layouts": []}
context = GLib.MainContext.default()


def command(*arguments):
    report["actions"].append({"atMonotonic": time.monotonic(), "command": list(arguments)})
    return subprocess.check_output(arguments, text=True, timeout=8)


def until(predicate, description):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        for _ in range(64):
            if not context.pending():
                break
            context.iteration(False)
        result = predicate()
        if result:
            report["checks"].append(description)
            return result
        time.sleep(.05)
    raise AssertionError(description)


def require(condition, description):
    if not condition:
        raise AssertionError(description)
    report["checks"].append(description)


def children(obj):
    return [obj.getChildAtIndex(i) for i in range(obj.childCount)]


def walk(obj):
    stack = [obj]
    for _ in range(5000):
        if not stack:
            return
        current = stack.pop()
        yield current
        stack.extend(child for child in reversed(children(current)) if child)
    raise AssertionError("accessible tree exceeds prototype node budget")


def rect(node, coordinates=pyatspi.WINDOW_COORDS):
    # Layout uses window-relative extents: GTK 4 on X11 reports a zero origin
    # for every control in desktop coordinates, which would make containment
    # and overlap checks vacuous. Desktop extents are used only where needed.
    value = node.queryComponent().getExtents(coordinates)
    return {key: int(getattr(value, key)) for key in ("x", "y", "width", "height")}


try:
    app = until(lambda: next((a for a in children(pyatspi.Registry.getDesktop(0))
                             if a and a.name == args.application), None), "application exported")
    nodes = list(walk(app))
    report["initialTree"] = [{"name": n.name, "role": n.getRoleName()} for n in nodes]
    frame = next(n for n in nodes if n.getRoleName() == "frame")

    def find(name, roles):
        return next(n for n in walk(app) if n.name.replace("_", "").replace("&", "") == name
                    and n.getRoleName() in roles)

    name = find("Name", ("entry", "text"))
    application = find("Application", ("entry", "text"))
    profiles = find("Profiles", ("list", "list box"))
    save = find("Save", ("push button", "button"))

    def text_node(node):
        # GTK may expose an empty Text wrapper whose child holds the editable
        # contents; use the candidate with the most characters (first on ties).
        best = None
        seen = report.setdefault("textCandidates", {}).setdefault(node.name, [])
        for candidate in walk(node):
            try:
                text = candidate.queryText()
            except NotImplementedError:
                continue
            if len(seen) < 8:
                seen.append({"role": candidate.getRoleName(), "name": candidate.name,
                             "characters": text.characterCount})
            if best is None or text.characterCount > best[1].characterCount:
                best = (candidate, text)
        if best is None:
            raise AssertionError(node.name + " exposes no Text interface")
        return best

    def current_text(node):
        # GTK rejects the -1 end sentinel; request the explicit character range.
        _, text = text_node(node)
        return text.getText(0, text.characterCount)

    def store_snapshot():
        # Every store entry, recursively, by relative path: regular files with
        # their exact bytes; directories and any other entry type by kind.
        entries = {}
        for path in sorted(args.store.rglob("*")):
            name = str(path.relative_to(args.store))
            if path.is_symlink():
                entries[name] = ("symlink", None)
            elif path.is_file():
                entries[name] = ("file", path.read_bytes())
            elif path.is_dir():
                entries[name] = ("dir", None)
            else:
                entries[name] = ("other", path.stat().st_mode)
        return entries

    def preserves(original_value, canonical_value, top=True):
        # The canonical form may add explicit defaults and migrate the document
        # to the current schemaVersion, but must keep every original value.
        if top and isinstance(original_value, dict) and isinstance(canonical_value, dict) and \
                "schemaVersion" in original_value:
            if canonical_value.get("schemaVersion", 0) < original_value["schemaVersion"]:
                return False
            original_value = {k: v for k, v in original_value.items() if k != "schemaVersion"}
        if isinstance(original_value, dict):
            return isinstance(canonical_value, dict) and all(
                key in canonical_value and preserves(value, canonical_value[key], False)
                for key, value in original_value.items())
        if isinstance(original_value, list):
            return isinstance(canonical_value, list) and len(original_value) == len(canonical_value) and all(
                preserves(x, y, False) for x, y in zip(original_value, canonical_value))
        return original_value == canonical_value

    def documents(snapshot):
        return {name: json.loads(entry[1]) for name, entry in snapshot.items()
                if entry[0] == "file" and name.endswith(".json") and "/" not in name}

    def profile_names(snapshot):
        return [document.get("name") for document in documents(snapshot).values()]

    command("xdotool", "windowactivate", "--sync", args.window)
    # Native mnemonic focus also exercises toolkits that do not implement the
    # optional AT-SPI Component.grabFocus method. The bridge's semantic focus
    # API is checked separately by bridge_test.py.
    command("xdotool", "key", "--clearmodifiers", "alt+n")
    until(lambda: name.getState().contains(pyatspi.STATE_FOCUSED), "Name mnemonic focus reflected")

    # Native keyboard edit: caret movement, typing and deletion are delivered
    # by the X server to the focused control, then saved through the store.
    original = current_text(name)
    # Save the unchanged document first: the store writes its canonical form
    # (explicit defaults), which becomes the byte baseline for the edit below.
    raw = store_snapshot()
    selected = [name for name, document in documents(raw).items() if document.get("name") == original]
    require(len(selected) == 1, f"fixture contains the selected profile once ({original!r})")
    selected = selected[0]
    baseline_backup = selected + ".bak"
    command("xdotool", "key", "--clearmodifiers", "alt+s")
    before = until(lambda: (lambda snap: snap if baseline_backup in snap else None)(store_snapshot()),
                   "unchanged Save writes the canonical baseline and its backup")
    # The baseline Save is validated independently: nothing removed, only the
    # selected document's backup added (holding its exact original bytes),
    # every other entry identical, and every original value preserved.
    require(not (raw.keys() - before.keys()) and before.keys() - raw.keys() == {baseline_backup},
            "baseline Save added only the selected document's backup and removed nothing")
    require(before[baseline_backup] == raw[selected], "baseline backup holds the exact original bytes")
    require(all(before[entry] == raw[entry] for entry in raw if entry != selected),
            "baseline Save left every other store entry identical")
    def differences(original_value, canonical_value, where=""):
        if isinstance(original_value, dict) and isinstance(canonical_value, dict):
            return [d for key, value in original_value.items()
                    for d in (differences(value, canonical_value[key], f"{where}/{key}")
                              if key in canonical_value else [f"{where}/{key}: missing"])]
        if isinstance(original_value, list) and isinstance(canonical_value, list) and \
                len(original_value) == len(canonical_value):
            return [d for i, (x, y) in enumerate(zip(original_value, canonical_value))
                    for d in differences(x, y, f"{where}/{i}")]
        return [] if original_value == canonical_value else [f"{where}: {original_value!r} -> {canonical_value!r}"]

    report["baselineDifferences"] = differences(json.loads(raw[selected][1]), json.loads(before[selected][1]))[:20]
    require(preserves(json.loads(raw[selected][1]), json.loads(before[selected][1])),
            "baseline Save preserved every original value of the selected document")
    command("xdotool", "key", "--clearmodifiers", "alt+n")
    until(lambda: name.getState().contains(pyatspi.STATE_FOCUSED), "Name focus restored after Save")
    require(profile_names(before).count(original) == 1,
            f"fixture contains the selected profile once ({original!r} in {profile_names(before)})")
    command("xdotool", "key", "--clearmodifiers", "End")
    command("xdotool", "type", "--delay", "40", "Zq")
    command("xdotool", "key", "--clearmodifiers", "BackSpace")
    edited = original + "Z"
    until(lambda: current_text(name) == edited, "native typing and deletion reflected in accessible text")
    command("xdotool", "key", "--clearmodifiers", "Home")
    until(lambda: text_node(name)[1].caretOffset == 0, "native Home moves the caret to the start")
    command("xdotool", "key", "--clearmodifiers", "End")
    until(lambda: text_node(name)[1].caretOffset == len(edited), "native End moves the caret to the end")
    command("xdotool", "key", "Tab")
    until(lambda: application.getState().contains(pyatspi.STATE_FOCUSED), "Tab reaches Application")
    command("xdotool", "key", "--clearmodifiers", "alt+s")
    until(lambda: any(n.name == "Saved" for n in walk(app)), "native Save mnemonic reports Saved")
    after = until(lambda: (lambda snap: snap if edited in profile_names(snap) else None)(store_snapshot()),
                  "keyboard edit committed to the disposable store")
    # Exactly the selected document changed, and only its name: every other
    # store entry is byte-identical and no entry was added or removed.
    report["storeEntries"] = {"added": sorted(after.keys() - before.keys()),
                               "removed": sorted(before.keys() - after.keys())}
    changed = [entry for entry in before if entry in after and after[entry] != before[entry]
               and not entry.endswith(".bak")]
    require(len(changed) == 1, f"Save rewrote exactly one store entry ({changed})")
    # The editor keeps the previous document as NAME.bak; nothing else may
    # appear, and nothing may disappear.
    backup = changed[0] + ".bak"
    require(not (before.keys() - after.keys()) and after.keys() - before.keys() <= {backup},
            "Save added only the edited document's backup and removed nothing")
    require(after.get(backup) == before[changed[0]],
            "the backup holds the exact previous document bytes")
    require(all(after[entry] == before[entry] for entry in before
                if entry not in (changed[0], backup)), "every other store entry is byte-identical")
    require(changed[0] == selected, "the edited Save rewrote the selected document")
    old_document = json.loads(before[changed[0]][1])
    new_document = json.loads(after[changed[0]][1])
    report["savedDifference"] = {key: [old_document.get(key), new_document.get(key)]
                                 for key in old_document.keys() | new_document.keys()
                                 if old_document.get(key) != new_document.get(key)}
    require(old_document.get("name") == original and new_document == {**old_document, "name": edited},
            "Save changed only the selected profile's name")
    report["nativeEdit"] = {"original": original, "edited": edited, "file": changed[0]}
    command("xdotool", "key", "--clearmodifiers", "alt+a")
    until(lambda: application.getState().contains(pyatspi.STATE_FOCUSED), "Application mnemonic restores focus")

    def layout(label, fields, requested):
        raw_bounds = rect(frame)
        geometry = dict(line.split("=", 1) for line in
                        command("xdotool", "getwindowgeometry", "--shell", args.window).splitlines()
                        if "=" in line)
        native_info = command("xwininfo", "-id", args.window)
        native_origin = {}
        for line in native_info.splitlines():
            for axis in ("X", "Y"):
                if "Absolute upper-left " + axis + ":" in line:
                    native_origin[axis] = int(line.rsplit(":", 1)[1].strip())
        factor = 1
        if args.application == "layere_proto_gtk" and abs(raw_bounds["width"] - int(geometry["WIDTH"])) > 4:
            # GTK's X11 AT-SPI extents can be in GDK logical coordinates.
            # Preserve the raw values and normalize only for layout comparison.
            factor = args.scale
        bounds = {key: int(value * factor) for key, value in raw_bounds.items()}
        controls = []
        report["layouts"].append({"scenario": label, "frame": bounds, "requested": requested,
                                  "rawFrame": raw_bounds, "extentScaleToDevice": factor,
                                  "controls": controls, "nativeGeometry": geometry,
                                  "nativeClientOrigin": native_origin})
        require(bounds["width"] > 0 and bounds["height"] > 0, label + ": frame has bounds")
        # A toolkit or WM may clamp a request to a larger natural minimum;
        # the scenario is only meaningful at the size actually requested.
        require(abs(int(geometry["WIDTH"]) - requested[0]) <= 4 and
                abs(int(geometry["HEIGHT"]) - requested[1]) <= 4,
                f"{label}: native window achieved the requested {requested[0]}x{requested[1]}")
        boxes = []
        for field in fields:
            raw_box = rect(field)
            box = {key: int(value * factor) for key, value in raw_box.items()}
            role = field.getRoleName()
            controls.append({"name": field.name, "role": role,
                             "bounds": box, "rawBounds": raw_box})
            if role in ("list", "list box"):
                # Scrollable content may extend past its viewport; check the
                # visible part, which must still start inside the window.
                require(bounds["x"] <= box["x"] < bounds["x"] + bounds["width"] and
                        bounds["y"] <= box["y"] < bounds["y"] + bounds["height"],
                        label + ": " + field.name + " starts inside frame")
                box = dict(box, width=min(box["width"], bounds["x"] + bounds["width"] - box["x"]),
                           height=min(box["height"], bounds["y"] + bounds["height"] - box["y"]))
                controls[-1]["visibleBounds"] = box
            require(field.getState().contains(pyatspi.STATE_SHOWING), label + ": " + field.name + " visible")
            # Text/numeric fields need room for content; buttons need at least
            # the WCAG 2.2 24x24 target; lists need two rows.
            minimum = {"push button": (24, 24), "button": (24, 24),
                       "list": (70, 44), "list box": (70, 44)}.get(role, (70, 22))
            require(box["width"] >= minimum[0] * args.scale and box["height"] >= minimum[1] * args.scale,
                    label + ": " + field.name + " readable control dimensions")
            require(bounds["x"] <= box["x"] and bounds["y"] <= box["y"] and
                    box["x"] + box["width"] <= bounds["x"] + bounds["width"] and
                    box["y"] + box["height"] <= bounds["y"] + bounds["height"],
                    label + ": " + field.name + " inside frame")
            boxes.append((field.name, box))
        require(len({(b["x"], b["y"]) for _, b in boxes}) == len(boxes),
                label + ": controls report distinct positions (geometry is not vacuous)")
        for index, (first, a) in enumerate(boxes):
            for second, b in boxes[index + 1:]:
                require(a["x"] + a["width"] <= b["x"] or b["x"] + b["width"] <= a["x"] or
                        a["y"] + a["height"] <= b["y"] or b["y"] + b["height"] <= a["y"],
                        label + ": " + first + " and " + second + " do not overlap")
        require(abs(bounds["width"] - int(geometry["WIDTH"])) <= 4 and
                abs(bounds["height"] - int(geometry["HEIGHT"])) <= 4,
                label + ": normalized AT-SPI dimensions match native X11 geometry")
        require(bounds["x"] == 0 and bounds["y"] == 0, label + ": window-relative frame origin")
        if args.application == "layere_proto_fltk_a11y":
            desktop = rect(frame, pyatspi.DESKTOP_COORDS)
            require(abs(desktop["x"] - native_origin["X"]) <= 2 and
                    abs(desktop["y"] - native_origin["Y"]) <= 2,
                    label + ": bridge origin matches native client, excluding decorations")
        command("import", "-window", "root", str(args.output / (label + ".png")))

    for label, width, height in (("default", 900, 600), ("minimum", 760, 400),
                                 ("enlarged", 1100, 720), ("restored", 900, 600)):
        requested = (int(width * args.scale), int(height * args.scale))
        command("xdotool", "windowsize", "--sync", args.window, *map(str, requested))
        time.sleep(.25)
        until(lambda: application.getState().contains(pyatspi.STATE_FOCUSED),
              label + ": native resize retains Application focus")
        layout(label + "-profile", (name, application, profiles, save), requested)

    if args.application == "layere_proto_gtk":
        # GTK page tabs expose selection, but not an AT-SPI Action interface.
        command("xdotool", "key", "--clearmodifiers", "ctrl+Page_Down")
        until(lambda: find("Cooldowns", ("page tab",)).getState().contains(pyatspi.STATE_SELECTED),
              "native keyboard selects Cooldowns tab")
    else:
        require(find("Cooldowns", ("page tab",)).queryAction().doAction(0),
                "Cooldown tab accessible action succeeds")
    cooldown_name = until(lambda: next((n for n in walk(app) if n.name.replace("_", "") == "Cooldown name"
                                       and n.getRoleName() in ("entry", "text")), None),
                          "Cooldown name exported after tab activation")
    duration = find("Duration (s)", ("spin button",))
    charges = find("Charges", ("spin button",))
    cooldown_list = find("Cooldowns", ("list", "list box"))
    until(lambda: duration.getState().contains(pyatspi.STATE_SHOWING), "numeric controls become visible")

    # Key capture is explicitly armed, cancellable, and never traps keyboard
    # navigation. All keys below are delivered natively to the focused control.
    capture = find("Capture key", ("push button", "button"))

    def focused(node):
        return node.getState().contains(pyatspi.STATE_FOCUSED)

    def status(text):
        return any(n.name == text for n in walk(app))

    command("xdotool", "key", "--clearmodifiers", "alt+n")
    until(lambda: focused(cooldown_name), "Cooldown name mnemonic focus")
    for _ in range(8):
        if focused(capture):
            break
        command("xdotool", "key", "Tab")
        time.sleep(.1)
    until(lambda: focused(capture), "forward Tab traversal reaches Capture key")
    armed = "Press a focused key; Escape or Tab cancels."
    cancelled = "Capture cancelled."
    command("xdotool", "key", "space")
    until(lambda: status(armed), "Space arms key capture")
    command("xdotool", "key", "Escape")
    until(lambda: status(cancelled) and focused(capture), "Escape cancels capture and keeps focus")
    command("xdotool", "key", "space")
    until(lambda: status(armed), "capture re-armed")
    command("xdotool", "key", "Tab")
    until(lambda: status(cancelled) and not focused(capture), "Tab cancels capture and leaves the control")
    # Toolkits differ in order (FLTK 1.4 visits the focusable tab header), so
    # require a bounded reverse path back, which also rules out a focus trap.
    def focus_name():
        return next(((n.getRoleName(), n.name) for n in walk(app) if focused(n)), None)

    reverse = report.setdefault("reverseFocusPath", [focus_name()])
    for _ in range(8):
        command("xdotool", "key", "shift+Tab")
        time.sleep(.15)
        reverse.append(focus_name())
        if focused(capture):
            break
    until(lambda: focused(capture), "bounded reverse Tab traversal returns to Capture key")
    def label_values():
        # A label named by a mnemonic relation (GTK) exposes its content as Text;
        # FLTK's bridge exposes a box's text as its name.
        values = []
        for node in walk(app):
            if node.getRoleName() != "label":
                continue
            try:
                text = node.queryText()
                value = text.getText(0, text.characterCount)
            except NotImplementedError:
                value = ""
            values.append(value or node.name)
        return values

    captured = "Captured focused key (prototype display only)."
    statuses = {armed, cancelled, captured, "Ready", "Saved"}
    binding_index = None
    # Two distinct keys must each change the same binding label to their exact
    # key symbol (k = 107, m = 109); no other non-status label may change.
    for key, expected in (("k", "Key 107"), ("m", "Key 109")):
        command("xdotool", "key", "space")
        until(lambda: status(armed), f"capture armed for {key}")
        labels_before = label_values()
        command("xdotool", "key", key)
        until(lambda: status(captured), f"{key} captured")
        labels_after = until(lambda: (lambda v: v if expected in v else None)(label_values()),
                             f"{key} displayed as {expected}")
        require(len(labels_after) == len(labels_before), "capture keeps the label set stable")
        changed = [i for i, (old, new) in enumerate(zip(labels_before, labels_after))
                   if old != new and old not in statuses and new not in statuses]
        require(len(changed) == 1 and labels_after[changed[0]] == expected,
                f"{key} changed exactly the binding label to {expected} ({changed})")
        require(binding_index in (None, changed[0]), "both captures update the same binding label")
        binding_index = changed[0]
        report.setdefault("captures", []).append(
            {"key": key, "before": labels_before[changed[0]], "after": expected})
    command("xdotool", "key", "Tab")
    until(lambda: not focused(capture), "Tab leaves Capture key after a capture")
    for label, width, height in (("minimum", 760, 400), ("enlarged", 1100, 720)):
        requested = (int(width * args.scale), int(height * args.scale))
        command("xdotool", "windowsize", "--sync", args.window, *map(str, requested))
        time.sleep(.25)
        layout(label + "-cooldown", (cooldown_name, duration, charges, cooldown_list, save), requested)
    command("xdotool", "key", "--clearmodifiers", "alt+F4")
    report["passed"] = True
except Exception as error:
    report["error"] = f"{type(error).__name__}: {error}"
    report["traceback"] = traceback.format_exc()
finally:
    (args.output / "layout-test.json").write_text(json.dumps(report, indent=2) + "\n")
if not report["passed"]:
    raise SystemExit(report.get("error", "layout probe failed"))
print("Prototype layout/focus checks: PASS")
