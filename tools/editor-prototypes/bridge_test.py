#!/usr/bin/env python3
"""Bounded AT-SPI probe, only for a disposable Xvfb/D-Bus prototype session."""
import argparse
import json
import subprocess
import time
import traceback
from pathlib import Path

import gi
import pyatspi

gi.require_version("Atspi", "2.0")
from gi.repository import GLib  # noqa: E402 -- version selection precedes the GI import

parser = argparse.ArgumentParser()
parser.add_argument("output", type=Path)
parser.add_argument("store", type=Path)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
events = []
checks = []
context = GLib.MainContext.default()


def dispatch():
    for _ in range(64):
        if not context.pending():
            break
        context.iteration(False)


def until(predicate, description, seconds=8):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        dispatch()
        result = predicate()
        if result:
            checks.append(description)
            return result
        time.sleep(.05)
    raise AssertionError(description)


def children(obj):
    return [obj.getChildAtIndex(i) for i in range(obj.childCount)]


def walk(obj):
    yield obj
    for child in children(obj):
        if child:
            yield from walk(child)


def app_lookup():
    return next((a for a in children(pyatspi.Registry.getDesktop(0))
                 if a and a.name == "layere_proto_fltk_a11y"), None)


def record(event):
    try:
        events.append({"type": event.type, "source": event.source.name,
                       "detail1": event.detail1, "detail2": event.detail2})
    except Exception as exc:
        events.append({"type": event.type, "error": str(exc)})


report = {"passed": False, "checks": checks, "events": events}
try:
    pyatspi.Registry.registerEventListener(
        record, "object:state-changed", "object:text-changed",
        "object:property-change:accessible-name")
    app = until(app_lookup, "application exported")
    nodes = list(walk(app))
    report["initial_tree"] = [{"name": n.name, "role": n.getRoleName()} for n in nodes]

    def find(name, role=None):
        return next(n for n in nodes if n.name == name and
                    (role is None or n.getRoleName() == role))

    profiles = find("Profiles", "list")
    cooldowns = find("Cooldowns", "list")
    assert profiles.childCount == 20 and cooldowns.childCount == 10
    checks.append("20 profile / 10 cooldown accessible children")
    first, second = children(profiles)[:2]
    assert first.name.startswith("Fixture 01") and second.name.startswith("Fixture 02")
    name = find("Name", "entry")
    application = find("Application", "entry")
    text = name.queryText()
    assert text.getText(0, -1) == "Fixture 01"
    checks.append("field text readable")
    window = subprocess.check_output(
        ["xdotool", "search", "--name", "LayerC Editor"], text=True).split()[0]
    subprocess.run(["xdotool", "windowactivate", "--sync", window], check=True)
    assert name.queryComponent().grabFocus()
    until(lambda: name.getState().contains(pyatspi.STATE_FOCUSED), "accessible focus reflected")
    subprocess.run(["xdotool", "key", "Tab"], check=True)
    until(lambda: application.getState().contains(pyatspi.STATE_FOCUSED), "keyboard Tab focus reflected")
    assert second.queryAction().doAction(0)
    until(lambda: second.getState().contains(pyatspi.STATE_SELECTED) and
          not first.getState().contains(pyatspi.STATE_SELECTED),
          "accessible profile selection reflected")
    until(lambda: text.getText(0, -1) == "Fixture 02", "selection updates field text")
    replacement = "Bridge Café Ω"
    name.queryEditableText().setTextContents(replacement)
    until(lambda: text.getText(0, -1) == replacement, "UTF-8 field update readable")
    assert text.characterCount == len(replacement)
    assert text.getText(7, 11) == replacement[7:11]
    assert text.setCaretOffset(8)
    assert text.caretOffset == 8
    checks.append("Unicode character offsets and caret")
    save = find("Save", "button")
    assert save.queryAction().doAction(0)
    profile_file = args.store / "fixture-02.json"
    until(lambda: json.loads(profile_file.read_text())["name"] == replacement,
          "accessible Save commits disposable profile")
    until(lambda: any(n.name == "Saved" for n in nodes), "saved status readable")
    until(lambda: any(e.get("type", "").startswith("object:state-changed:focused") and
                      e.get("detail1") == 1 for e in events), "focus event emitted")
    until(lambda: any(e.get("type", "").startswith("object:text-changed:insert") for e in events),
          "text-change event emitted")
    until(lambda: any(e.get("type", "").startswith("object:state-changed:selected") for e in events),
          "selection event emitted")
    report["tree"] = [{"name": n.name, "role": n.getRoleName()} for n in walk(app)]
    subprocess.run(["xdotool", "key", "--clearmodifiers", "alt+F4"], check=True, timeout=5)
    report["passed"] = True
except Exception as exc:
    report["error"] = f"{type(exc).__name__}: {exc}"
    report["traceback"] = traceback.format_exc()
finally:
    dispatch()
    (args.output / "bridge-test.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    pyatspi.Registry.deregisterEventListener(record, "object:state-changed",
        "object:text-changed", "object:property-change:accessible-name")
if not report["passed"]:
    raise SystemExit(report.get("error", "bridge probe failed"))
print("AT-SPI bridge checks: PASS")
