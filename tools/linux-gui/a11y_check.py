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


def profile_fixture_index(row_names, expected_names, fixture_name):
    """Verify the whole roster, then locate the named fixture without an order assumption."""
    # Keep authored name whitespace; only the shared profile_list_text separator is decoration.
    names = [name.partition("  —  ")[0] for name in row_names]
    assert sorted(names) == sorted(expected_names), f"profile rows: {names}; expected {expected_names}"
    matches = [index for index, name in enumerate(names) if name == fixture_name]
    assert len(matches) == 1, f"fixture row is ambiguous or missing: {fixture_name}; {names}"
    return matches[0]


def main():
    profile = Path(sys.argv[1]) / "fixture.json"
    import importlib  # noqa: PLC0415 -- Linux-only dependencies are loaded inside the runner
    gi = importlib.import_module("gi")
    gi.require_version("Atspi", "2.0")
    GLib = importlib.import_module("gi.repository.GLib")
    pyatspi = importlib.import_module("pyatspi")

    context = GLib.MainContext.default()
    events, checks = [], []

    def dispatch():
        for _ in range(1024):
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

    # Searches use lists and plain loops, never suspended or abandoned
    # generators: releasing an accessible can dispatch AT-SPI event callbacks,
    # and Python 3.13 crashed when one ran while a nested walk generator was
    # being finalized.
    def walk(node):
        nodes, pending = [], [node]
        while pending:
            current = pending.pop()
            nodes.append(current)
            pending.extend(reversed([c for c in children(current) if c is not None]))
        return nodes

    def first(nodes, predicate):
        for node in nodes:
            if node is not None and predicate(node):
                return node
        return None

    def application():
        return first(children(pyatspi.Registry.getDesktop(0)), lambda a: a.name == "LayerE")

    def lookup(name, role, root=None):
        root = root if root is not None else application()
        if root is None:
            return None
        return first(walk(root), lambda n: n.name == name and n.getRoleName() == role)

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
        frame = until(lambda: first(children(app), lambda c: c.getRoleName() == "frame"),
                      "main window exported")
        until(lambda: lookup("Accessibility: AT-SPI bridge active", "label"),
              "visible bridge status exported")
        menus = [m.name for m in children(find("Main menu", "menu bar"))]
        assert menus == ["File", "Edit", "Profile", "Cooldowns", "Bindings", "Panels", "View", "Tools",
                         "Help"], menus
        save_item = find("Save", "menu item")
        assert "<Control>s" in save_item.queryAction().getKeyBinding(0)
        checks.append("menu bar, items and Ctrl+S keybinding exported")

        saved_name = json.loads(profile.read_text())["name"]
        profiles = find("Profiles", "list")
        rows = children(profiles)
        expected_names = [json.loads(path.read_text())["name"]
                          for path in profile.parent.glob("*.json")]
        fixture = profile_fixture_index([row.name for row in rows], expected_names, saved_name)
        selection = profiles.querySelection()
        assert selection.selectChild(fixture), "fixture row could not be selected"
        until(lambda: selection.nSelectedChildren == 1 and selection.isChildSelected(fixture),
              "fixture profile selected through accessibility")
        name = find("Profile name", "entry")
        text = name.queryText()
        until(lambda: text.getText(0, -1) == saved_name, "selected profile name field readable")
        checks.append("profile name field readable")
        checks.append("complete profile list exported and fixture selected")

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
        dialog = until(lambda: first(children(application()),
                                     lambda c: c.getRoleName() == "dialog"),
                       "modal Delete dialog exported")
        until(lambda: state(dialog, pyatspi.STATE_MODAL), "dialog is modal")
        # A modal disables every main-window control, so its creation event
        # follows a burst of state changes; observe it while the dialog lives.
        until(lambda: seen("window:create", role="dialog"), "dialog creation event emitted")
        prompt = [n.name for n in walk(dialog) if n.getRoleName() == "label"]
        assert any("Delete this profile" in p for p in prompt), prompt
        cancel = find("Cancel", "button", dialog)
        assert cancel.queryAction().doAction(0)
        until(lambda: first(children(application()),
                            lambda c: c.getRoleName() == "dialog") is None,
              "dialog closed by accessible Cancel")
        assert profile.is_file()
        checks.append("cancelled Delete kept the profile")

        until(lambda: seen("object:state-changed:focused", detail1=1), "focus event emitted")
        until(lambda: seen("object:text-changed:insert"), "text-change event emitted")
        until(lambda: seen("object:state-changed:selected", detail1=1), "selection event emitted")
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
