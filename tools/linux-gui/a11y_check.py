#!/usr/bin/env python3
"""Semantic AT-SPI walkthrough of the production LayerE (one JSON object).

Run by the bridge's `a11y_check` operation inside a disposable session, on the
GUI fixture profile. It drives the editor only through accessibility
interfaces, plus one keyboard Tab on the runner-owned display, and verifies
that names, focus, text, selection, actions, events and a modal dialog are
exported. The fixture profile is renamed and saved; Delete is cancelled.
"""
import json
import subprocess
import sys
import time
import traceback
from pathlib import Path

REPLACEMENT = "Bridge Café Ω"
EVENTS = ("object:state-changed", "object:text-changed",
          "object:property-change:accessible-name", "object:children-changed",
          "object:selection-changed", "object:text-caret-moved", "window:create",
          "window:destroy", "window:activate")


def main():
    profile = Path(sys.argv[1]) / "fixture.json"
    import gi  # noqa: PLC0415 -- the bridge reports absence instead of failing
    gi.require_version("Atspi", "2.0")
    from gi.repository import GLib  # noqa: PLC0415
    import pyatspi  # noqa: PLC0415

    context = GLib.MainContext.default()
    events, checks = [], []

    def dispatch():
        for _ in range(64):
            if not context.pending():
                break
            context.iteration(False)

    def until(predicate, description, seconds=8.0):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            dispatch()
            result = predicate()
            if result:
                checks.append(description)
                return result
            time.sleep(0.05)
        raise AssertionError(description)

    def children(node):
        return [node.getChildAtIndex(i) for i in range(node.childCount)]

    def walk(node):
        yield node
        for child in children(node):
            if child is not None:
                yield from walk(child)

    def application():
        return next((a for a in children(pyatspi.Registry.getDesktop(0))
                     if a is not None and a.name == "LayerE"), None)

    def lookup(name, role, root=None):
        return next((n for n in walk(root or application())
                     if n.name == name and n.getRoleName() == role), None)

    def find(name, role, root=None):
        node = lookup(name, role, root)
        if node is None:
            raise AssertionError(f"{role} {name!r} is not exported")
        return node

    def state(node, kind):
        return node.getState().contains(kind)

    def record(event):
        try:
            events.append({"type": event.type, "source": event.source.name,
                           "role": event.source.getRoleName(), "detail1": event.detail1})
        except Exception as error:  # noqa: BLE001 -- the source may be defunct
            events.append({"type": event.type, "error": str(error)})

    def seen(prefix, **match):
        return any(e.get("type", "").startswith(prefix) and
                   all(e.get(k) == v for k, v in match.items()) for e in events)

    report = {"passed": False, "checks": checks, "events": events}
    pyatspi.Registry.registerEventListener(record, *EVENTS)
    try:
        app = until(application, "application exported as LayerE")
        frame = until(lambda: next((c for c in children(app) if c.getRoleName() == "frame"), None),
                      "main window exported")
        until(lambda: lookup("Accessibility: AT-SPI bridge active", "label"),
              "visible bridge status exported")
        menus = [m.name for m in children(find("Main menu", "menu bar"))]
        assert menus == ["File", "Edit", "Profile", "Cooldowns", "Bindings", "Panels", "View"], menus
        save_item = find("Save", "menu item")
        assert "<Control>s" in save_item.queryAction().getKeyBinding(0)
        checks.append("menu bar, items and Ctrl+S keybinding exported")

        name = find("Profile name", "entry")
        text = name.queryText()
        saved_name = json.loads(profile.read_text())["name"]
        assert text.getText(0, -1) == saved_name, text.getText(0, -1)
        checks.append("profile name field readable")
        rows = children(find("Profiles", "list"))
        assert len(rows) == 1 and state(rows[0], pyatspi.STATE_SELECTED)
        checks.append("profile list row exported and selected")

        windows = subprocess.check_output(["xdotool", "search", "--name", "^LayerE"],
                                          text=True, timeout=5).split()
        assert len(windows) == 1, f"expected one LayerE window, found {windows}"
        window = windows[0]
        subprocess.run(["xdotool", "windowactivate", "--sync", window], check=True, timeout=5)
        assert name.queryComponent().grabFocus()
        until(lambda: state(name, pyatspi.STATE_FOCUSED), "accessible focus reflected")
        subprocess.run(["xdotool", "key", "--clearmodifiers", "Tab"], check=True, timeout=5)
        executable = find("Executable name", "entry")
        until(lambda: state(executable, pyatspi.STATE_FOCUSED), "keyboard Tab focus reflected")

        name.queryEditableText().setTextContents(REPLACEMENT)
        until(lambda: text.getText(0, -1) == REPLACEMENT, "UTF-8 field edit readable")
        assert text.characterCount == len(REPLACEMENT)
        assert text.getText(7, 11) == REPLACEMENT[7:11]
        assert text.setCaretOffset(8) and text.caretOffset == 8
        checks.append("Unicode character offsets and caret")
        until(lambda: frame.name.endswith("*"), "unsaved title exported")
        undo = find("Undo", "button")
        until(lambda: state(undo, pyatspi.STATE_ENABLED), "Undo enablement exported")

        cooldowns = find("Cooldowns", "page tab")
        assert cooldowns.queryAction().doAction(0)
        until(lambda: state(cooldowns, pyatspi.STATE_SELECTED), "page tab selected by action")
        cooldown_name = find("Cooldown name", "entry")
        until(lambda: state(cooldown_name, pyatspi.STATE_SHOWING), "selected page content showing")
        recovery = find("Recovery mode", "combo box").querySelection()
        assert recovery.nSelectedChildren == 1 and recovery.getSelectedChild(0).name
        checks.append(f"combo selection exported ({recovery.getSelectedChild(0).name})")

        assert save_item.queryAction().doAction(0)
        until(lambda: json.loads(profile.read_text())["name"] == REPLACEMENT,
              "accessible menu Save commits the profile")
        outcome = find("Save / reload outcome", "text").queryText()
        until(lambda: "Saved" in outcome.getText(0, -1), "save outcome readable")
        until(lambda: not frame.name.endswith("*"), "saved title exported")

        assert find("Delete...", "menu item").queryAction().doAction(0)
        dialog = until(lambda: next((c for c in children(application())
                                     if c.getRoleName() == "dialog"), None),
                       "modal Delete dialog exported")
        until(lambda: state(dialog, pyatspi.STATE_MODAL), "dialog is modal")
        prompt = [n.name for n in walk(dialog) if n.getRoleName() == "label"]
        assert any("Delete this profile" in p for p in prompt), prompt
        cancel = find("Cancel", "button", dialog)
        assert cancel.queryAction().doAction(0)
        until(lambda: not any(c.getRoleName() == "dialog" for c in children(application())),
              "dialog closed by accessible Cancel")
        assert profile.is_file()
        checks.append("cancelled Delete kept the profile")

        until(lambda: seen("object:state-changed:focused", detail1=1), "focus event emitted")
        until(lambda: seen("object:text-changed:insert"), "text-change event emitted")
        until(lambda: seen("object:state-changed:selected", detail1=1), "selection event emitted")
        until(lambda: seen("window:create", role="dialog"), "dialog creation event emitted")
        report["passed"] = True
    except Exception as error:  # noqa: BLE001 -- reported, never raised to the bridge
        report["error"] = f"{type(error).__name__}: {error}"
        report["traceback"] = traceback.format_exc()
    finally:
        dispatch()
        pyatspi.Registry.deregisterEventListener(record, *EVENTS)
    events[:] = events[-60:]
    print(json.dumps(report, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
