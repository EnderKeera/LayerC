#!/usr/bin/env python3
"""Scripted walkthrough of the Linux LayerE X11 editor, driven through the bridge.

Uses exactly the protocol an interactive agent uses: capture, cite the fresh
screenshot for one action, capture again, then verify the accessibility tree,
pixels, the durable profile store and daemon state. Targets are found by
accessible role and name; every action is real X input except the explicit
semantic accessibility check. Writes cases.json; exits nonzero unless every
required case passes. A failure leaves later cases blocked, never passed.
"""
import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from automation_walkthrough import EXPECTED as AUTOMATION_EXPECTED
from automation_walkthrough import AutomationWalk  # noqa: E402
from bridge import call  # noqa: E402

EDITOR = "LayerE"  # Title prefix; the title also names the open profile.
GAME = "LayerC Automation Fixture"
CYAN = [80, 186, 200]
MINIMUM = (640, 480)
PROFILE = "fixture.json"
STATUS = "Save / reload outcome"
DAEMON = "Daemon status and capability faults"
ROW = 20  # Profile-list row pitch at the default scale.

EXPECTED = {
    "bridge-guards": "Bad token, out-of-screen input, stale screenshot and unknown "
                     "operations are rejected and logged without acting.",
    "startup": "Editor and game fixture are visible; the profile is a file; LayerD "
               "answers; LayerE exports an AT-SPI tree with its bridge status.",
    "accessibility-semantics": "Through AT-SPI alone: names, roles, focus, keyboard Tab, "
                               "Unicode text, tab/combo selection, menu Save and a "
                               "cancelled modal Delete, with the matching events.",
    "keyboard-navigation": "Tab/Shift+Tab move focus between fields; Ctrl+2/Ctrl+1 switch "
                           "pages; Alt+F opens the File menu and Escape closes it.",
    "menus-context": "The empty-list context menu leaves the document unchanged; the "
                     "selected-row context menu duplicates the profile; Profile > Delete "
                     "is cancelled with Escape, then confirmed, and removes the copy.",
    "profiles-crud": "Ctrl+N creates a profile that saves durably; the enabled check box "
                     "saves; undo/redo restore field text; a dirty selection change "
                     "prompts, cancels, then discards.",
    "invalid-no-write": "Duration 'nan' is reported as invalid while typing and after "
                        "Ctrl+S, and the durable profile stays byte-identical.",
    "edit-commit-reload-overlay": "Duration 3.25 s and panel X 0.75 are saved durably, "
                                  "LayerD reloads, and the live overlay moves right.",
    "import-export": "Export JSON through the file chooser writes the saved bytes; "
                     "import of the same file reports the collision, copies it on "
                     "request, and a cancelled import writes nothing.",
    **AUTOMATION_EXPECTED,
    "layout": "Corner drags grow the window and clamp it at 640x480 with scrolling; "
              "title-bar double-clicks maximize and restore; Ctrl+Plus/Ctrl+0 scale "
              "and restore the window.",
    "daemon-reconnect": "With LayerD stopped the status says so and Save commits offline; "
                        "after LayerD restarts the status recovers and a Save is "
                        "acknowledged.",
    "store-failure-visible": "When the store cannot replace the profile, Save fails "
                             "visibly and the original profile bytes survive.",
}
DEFERRED = {
    "overlay-target-changes": "target resize/focus and click-through checks belong to the "
                              "overlay integration walkthrough (runtime suites cover them)",
    "later-parity": "visual calibration and portal cases are Change 8",
}


class Failure(Exception):
    pass


def check(condition, message):
    if not condition:
        raise Failure(message)


def flatten(node, depth=0, out=None):
    out = [] if out is None else out
    if node:
        out.append({**{k: v for k, v in node.items() if k != "children"}, "depth": depth})
        for child in node.get("children", []):
            flatten(child, depth + 1, out)
    return out


def center(node):
    x, y, w, h = node["extents"]
    check(w > 0 and h > 0, f"{node['role']} {node['name']!r} has no on-screen extents")
    return x + w // 2, y + h // 2


class Walk(AutomationWalk):
    def __init__(self, session):
        self.session = session
        self.shot = {}
        self.shots = []
        self.notes = {}
        self.base_hash = None

    # Protocol -----------------------------------------------------------
    def request(self, op, **args):
        response = call(self.session, {"op": op, **args})
        check(response.get("ok"), f"{op} failed: {response.get('error')}")
        return response

    def expect_rejected(self, request, reason, token=None):
        response = call(self.session, request, token=token)
        check(response.get("rejected") and reason in response.get("error", ""),
              f"expected rejection containing {reason!r}, got {response}")

    def capture(self, name):
        self.shot = self.request("screenshot", name=name)
        self.shots.append(self.shot["relative"])
        return self.shot

    def act(self, op, label, **args):
        self.request(op, screenshot=self.shot["screenshot"], **args)
        return self.capture(label)

    def key(self, combo, label):
        return self.act("key", label, combo=combo)

    def window(self, prefix):
        for window in self.shot["windows"]:
            if window["name"].startswith(prefix):
                return window
        raise Failure(f"window not visible: {prefix}")

    def focus(self, prefix, label):
        return self.act("activate", label, window=self.window(prefix)["id"])

    # Accessibility tree ---------------------------------------------------
    def nodes(self):
        tree = self.request("tree")
        check(tree.get("available"), f"accessibility tree unavailable: {tree.get('reason')}")
        return flatten(tree["tree"])

    def find(self, role, name=None, nodes=None, **match):
        for node in nodes if nodes is not None else self.nodes():
            if node["role"] == role and (name is None or node["name"] == name) and \
                    all(node.get(k) == v for k, v in match.items()):
                return node
        raise Failure(f"{role} {name!r} is not exported")

    def until(self, predicate, description, seconds=5.0):
        deadline = time.monotonic() + seconds
        while True:
            nodes = self.nodes()
            result = predicate(nodes)
            if result:
                return result
            check(time.monotonic() < deadline, description)
            time.sleep(0.2)

    def text(self, name, role="text"):
        return self.find(role, name).get("text", "")

    def dialog(self):
        """The topmost dialog's subtree, or None."""
        nodes = self.nodes()
        for index, node in enumerate(nodes):
            if node["depth"] == 1 and node["role"] == "dialog":
                subtree = []
                for child in nodes[index + 1:]:
                    if child["depth"] <= 1:
                        break
                    subtree.append(child)
                return node, subtree
        return None

    def click(self, node, label, **args):
        x, y = center(node)
        return self.act("click", label, x=x, y=y, **args)

    def type_into(self, role, name, text, label):
        x, y = center(self.find(role, name))
        return self.act("set_text", label, x=x, y=y, text=text)

    def press(self, button, label):
        found = self.dialog()
        if found is None:
            raise Failure(f"no dialog for {button!r}")
        return self.click(self.find("button", button, found[1]), label)

    def choose(self, title, path, label):
        """Type a path into the dialog's filename field and accept it."""
        found = self.until(lambda _: self.dialog(), f"{title} chooser did not open")
        check(found[0]["name"].startswith(title), f"unexpected dialog {found[0]['name']!r}")
        entry = next((n for n in found[1] if n["role"] == "entry"), None)
        check(entry is not None, "file chooser has no filename field")
        x, y = center(entry)
        self.act("set_text", label + "-path", x=x, y=y, text=path)
        return self.key("Return", label)

    def row(self, index, label, button=1):
        """Click profile-list row `index` (0-based) by its position."""
        x, y, w, _ = self.find("list", "Profiles")["extents"]
        return self.act("click", label, x=x + w // 2, y=y + 11 + index * ROW, button=button)

    # Store and runtime ------------------------------------------------------
    def state(self):
        return self.request("state")

    def profile(self, name=PROFILE):
        state = self.state()
        entry = next((p for p in state["profiles"] if p["name"] == name), None)
        if entry is None:
            raise Failure(f"profile entry {name} is missing")
        return entry, state["daemon"]

    def store(self):
        return {p["name"]: p for p in self.state()["profiles"]}

    def save(self, label):
        """Ctrl+S; the visible outcome must change across the Save itself."""
        before = self.text(STATUS)
        self.key("ctrl+s", label)
        after = self.until(lambda n: (t := self.find("text", STATUS, n).get("text", "")) != before
                           and t, f"no visible outcome for {label}")
        return after

    def overlay_x(self):
        game = self.window(GAME)
        width, height = self.shot["screen"]
        region = [max(0, game["x"]), max(0, game["y"]),
                  min(game["width"], width - game["x"]), min(game["height"], height - game["y"])]
        found = self.request("inspect", screenshot=self.shot["screenshot"], rgb=CYAN, region=region)
        check(found["count"] > 100, f"overlay not visible over the game ({found['count']} px)")
        return found["meanX"]

    def editor_size(self):
        editor = self.window(EDITOR)
        return editor["width"], editor["height"]

    # Cases ------------------------------------------------------------------
    def case_bridge_guards(self):
        self.expect_rejected({"op": "state"}, "token", token="0" * 64)
        self.capture("guards")
        fresh = self.shot["screenshot"]
        self.expect_rejected({"op": "click", "screenshot": fresh, "x": 99999, "y": 5}, "must be an integer in")
        self.request("key", screenshot=fresh, combo="shift")  # Harmless action.
        self.expect_rejected({"op": "key", "screenshot": fresh, "combo": "shift"}, "predates")
        self.expect_rejected({"op": "launch_shell"}, "unknown operation")
        self.expect_rejected({"op": "inspect", "screenshot": fresh, "region": None}, "region")
        self.capture("guards-after")
        return "five invalid requests rejected; one harmless action performed"

    def case_startup(self):
        self.capture("startup")
        self.window(GAME)
        width, height = self.editor_size()
        check(width >= MINIMUM[0] and height >= MINIMUM[1], f"editor below minimum: {width}x{height}")
        entry, daemon = self.profile()
        check(entry["kind"] == "file" and daemon["reachable"], "store or daemon not ready")
        self.base_hash = entry["sha256"]
        nodes = self.nodes()
        self.find("label", "Accessibility: AT-SPI bridge active", nodes)
        self.find("menu bar", "Main menu", nodes)
        return f"editor {width}x{height}; {len(nodes)} accessible nodes; bridge active"

    def case_accessibility_semantics(self):
        # The semantic check reports through its response, not a screenshot.
        self.focus(EDITOR, "a11y-focused")
        report = self.request("a11y_check", screenshot=self.shot["screenshot"])
        self.capture("a11y-after")
        self.notes["a11y"] = {"checks": report.get("checks"), "error": report.get("error")}
        check(report.get("passed"), f"accessibility check failed: {report.get('error')}")
        self.base_hash = self.profile()[0]["sha256"]
        return f"{len(report.get('checks') or [])} semantic checks passed"

    def keyboard_focus(self, name, nodes):
        return self.find("entry", name, nodes).get("focused")

    def case_keyboard_navigation(self):
        self.focus(EDITOR, "nav-focused")
        self.key("ctrl+1", "nav-profile-page")
        self.click(self.find("entry", "Profile name"), "nav-name-clicked")
        self.until(lambda n: self.keyboard_focus("Profile name", n), "click did not focus the name")
        self.key("Tab", "nav-tab")
        self.until(lambda n: self.keyboard_focus("Executable name", n), "Tab did not reach Executable name")
        self.key("shift+Tab", "nav-shift-tab")
        self.until(lambda n: self.keyboard_focus("Profile name", n), "Shift+Tab did not return")
        self.key("ctrl+2", "nav-cooldowns")
        self.until(lambda n: self.find("page tab", "Cooldowns", n).get("selected"), "Ctrl+2 failed")
        self.key("ctrl+1", "nav-profile")
        self.until(lambda n: self.find("page tab", "Profile", n).get("selected"), "Ctrl+1 failed")
        before = self.store()
        self.key("alt+f", "nav-file-menu")
        self.key("Escape", "nav-menu-closed")
        check(self.store().keys() == before.keys(), "opening the File menu changed the store")
        return "Tab/Shift+Tab focus, Ctrl+2/Ctrl+1 pages, Alt+F menu opened and cancelled"

    def case_menus_context(self):
        self.focus(EDITOR, "menus-focused")
        before = self.store()
        x, y, w, h = self.find("list", "Profiles")["extents"]
        self.act("click", "context-empty", x=x + w // 2, y=y + h - 12, button=3)
        self.key("Escape", "context-empty-closed")
        check(self.store().keys() == before.keys(), "empty-area context menu changed the store")
        self.row(0, "context-row", button=3)
        for step in range(6):  # New, Import, Export JSON, Export pack, Save, Duplicate.
            self.key("Down", f"context-row-down-{step}")
        self.key("Return", "context-duplicate")
        self.until(lambda n: "(copy)" in n[1]["name"], "Duplicate did not open the copy")
        self.save("copy-saved")
        added = set(self.store()) - set(before)
        check(len(added) == 1, f"saving the duplicate added {sorted(added)}")
        copy = added.pop()
        self.key("alt+p", "profile-menu")
        self.key("Down", "profile-menu-duplicate")
        self.key("Down", "profile-menu-delete")
        self.key("Return", "delete-prompt")
        self.until(lambda _: self.dialog(), "Delete did not prompt")
        self.key("Escape", "delete-cancelled")
        check(copy in self.store() and self.dialog() is None, "cancelled Delete removed the copy")
        self.key("alt+p", "profile-menu-again")
        self.key("Down", "profile-menu-duplicate-again")
        self.key("Down", "profile-menu-delete-again")
        self.key("Return", "delete-prompt-again")
        self.until(lambda _: self.dialog(), "Delete did not prompt again")
        self.press("Delete", "delete-confirmed")
        self.until(lambda _: copy not in self.store(), "confirmed Delete kept the copy")
        return f"empty-area menu inert; row menu duplicated {copy}; Delete cancelled, then confirmed"

    def case_profiles_crud(self):
        self.focus(EDITOR, "crud-focused")
        before = set(self.store())
        self.key("ctrl+n", "crud-new")
        self.key("ctrl+1", "crud-profile-page")
        self.type_into("entry", "Profile name", "Walkthrough profile", "crud-named")
        self.type_into("entry", "Executable name", "walk.exe", "crud-executable")
        self.save("crud-saved")
        added = set(self.store()) - before
        check(len(added) == 1, f"new profile did not save one file: {sorted(added)}")
        created = added.pop()
        data = self.store()[created]["json"]
        check(data["name"] == "Walkthrough profile" and data["enabled"], f"saved {data}")
        self.click(self.find("check box", "Profile enabled"), "crud-disabled")
        self.save("crud-disabled-saved")
        check(not self.store()[created]["json"]["enabled"], "enabled flag not saved")
        self.type_into("entry", "Profile name", "Renamed once", "crud-renamed")
        self.key("Tab", "crud-rename-committed")
        self.key("ctrl+z", "crud-undo")
        self.until(lambda n: self.find("entry", "Profile name", n).get("text") == "Walkthrough profile",
                   "undo did not restore the name")
        self.key("ctrl+shift+z", "crud-redo")
        self.until(lambda n: self.find("entry", "Profile name", n).get("text") == "Renamed once",
                   "redo did not reapply the name")
        fixture = self.row_of(self.profile()[0]["json"]["name"])
        self.row(fixture, "crud-dirty-switch")
        self.until(lambda _: self.dialog(), "dirty selection change did not prompt")
        self.press("Cancel", "crud-switch-cancelled")
        check("Renamed once" in self.window(EDITOR)["name"], "Cancel left the edited profile")
        self.row(fixture, "crud-dirty-switch-again")
        self.until(lambda _: self.dialog(), "dirty selection change did not prompt again")
        self.press("Discard", "crud-discarded")
        self.until(lambda n: "Walkthrough" not in n[1]["name"] and "Renamed" not in n[1]["name"],
                   "Discard did not switch profiles")
        check(self.store()[created]["json"]["name"] == "Walkthrough profile",
              "discarded edit reached the store")
        self.notes["created"] = created
        return f"{created} created, disabled and saved; undo/redo; dirty switch cancelled then discarded"

    def profile_rows(self, name):
        """Rows in this list only; names can legitimately repeat after import."""
        profiles = self.find("list", "Profiles")
        nodes = self.nodes()
        start = next(i for i, n in enumerate(nodes)
                     if n["role"] == "list" and n["name"] == "Profiles")
        stop = next((i for i in range(start + 1, len(nodes))
                     if nodes[i]["depth"] <= profiles["depth"]), len(nodes))
        rows = [n for n in nodes[start + 1:stop] if n["depth"] == profiles["depth"] + 1
                and n["role"] == "list item"]
        # Match the shared profile_list_text decorations, not a bare prefix:
        # an imported "Fixture (copy)" must not stand in for "Fixture".
        return [index for index, node in enumerate(rows)
                if node["name"] == name or node["name"] == name + "  (off)"
                or node["name"].startswith(name + "  —  ")]

    def row_of(self, name):
        matches = self.profile_rows(name)
        if len(matches) == 1:
            return matches[0]
        raise Failure(f"profile row {name!r} is missing or ambiguous")

    def select_fixture(self, label):
        """Open the fixture profile through the list, by its current name."""
        profile = self.profile()[0]["json"]
        name = profile["name"]
        for index in self.profile_rows(name):
            self.row(index, label + f"-row-{index}")
            if self.text("Profile ID (read-only)") == profile["id"]:
                return
        raise Failure(f"fixture {name!r} with ID {profile['id']!r} did not open")

    def case_invalid_no_write(self):
        self.focus(EDITOR, "invalid-focused")
        self.select_fixture("invalid-fixture")
        self.base_hash = self.profile()[0]["sha256"]
        self.key("ctrl+2", "invalid-cooldowns")
        self.type_into("entry", "Duration (seconds)", "nan", "invalid-typed")
        typed = self.until(lambda n: "Invalid" in (t := self.find("text", STATUS, n).get("text", ""))
                           and t, "invalid duration was not reported while typing")
        self.key("ctrl+s", "invalid-save")
        refused = self.text(STATUS)
        check("Invalid" in refused, f"Save hid the invalid input: {refused!r}")
        check(self.profile()[0]["sha256"] == self.base_hash, "invalid input changed the durable profile")
        return f"{typed!r} before and after Ctrl+S; profile bytes unchanged"

    def case_edit_commit_reload_overlay(self):
        self.focus(GAME, "game-focused")
        self.key("e", "overlay-before")
        before = self.overlay_x()
        self.focus(EDITOR, "editor-refocused")
        self.key("ctrl+2", "overlay-cooldowns")
        self.type_into("entry", "Duration (seconds)", "3.25", "duration-typed")
        self.key("ctrl+4", "overlay-panels")
        self.type_into("entry", "X (0\u20131)", "0.75", "panel-typed")
        self.save("saved")
        entry, daemon = self.profile()
        profile = entry["json"] or {}
        check(profile["cooldowns"][0]["durationMs"] == 3250, "duration not saved")
        check(profile["panels"][0]["anchorX"] == 0.75, "panel X not saved")
        check(daemon["reachable"], "daemon stopped answering after reload")
        self.focus(GAME, "game-refocused")
        self.key("e", "overlay-after")
        after = self.overlay_x()
        check(after - before > 250, f"overlay did not move right: {before:.1f} -> {after:.1f}")
        self.base_hash = entry["sha256"]
        return f"durationMs 3250, anchorX 0.75; overlay meanX {before:.1f} -> {after:.1f}"

    def case_import_export(self):
        self.focus(EDITOR, "exchange-focused")
        state = self.state()
        exchange = state["exchange"]
        saved = self.profile()[0]["sha256"]
        self.key("ctrl+shift+e", "export-open")
        self.choose("Export", exchange + "/exported.json", "exported")
        files = self.state()["exchangeFiles"]
        check(files.get("exported.json") == saved, f"export did not write the saved bytes: {files}")
        before = set(self.store())
        self.key("ctrl+o", "import-cancel-open")
        # The chooser reopens in the export directory, which holds a non-UTF-8 name.
        self.until(lambda n: any("not-utf8" in x["name"] and "\ufffd" in x["name"] for x in n) or None,
                   "the chooser's non-UTF-8 file name was not exported as valid UTF-8")
        self.choose("Import", exchange + "/exported.json", "import-cancel-chosen")
        self.until(lambda _: self.dialog(), "import did not review the collision")
        self.press("Cancel import", "import-cancelled")
        check(set(self.store()) == before, "a cancelled import wrote profiles")
        self.key("ctrl+o", "import-open")
        self.choose("Import", exchange + "/exported.json", "import-chosen")
        prompts = []
        for button in ("Include", "Copy", "Import"):
            found = self.until(lambda _: self.dialog(), f"import stopped before {button}")
            prompts.append(" | ".join(n["name"] for n in found[1] if n["role"] == "label"
                                      and n["name"] != "?"))
            self.press(button, f"import-{button.lower()}")
        check("collision" in prompts[0], f"collision was not reported: {prompts[0]!r}")
        added = set(self.until(lambda _: set(self.store()) - before or None,
                               "import added no profile"))
        check(len(added) == 1, f"import added {sorted(added)}")
        self.notes["imported"] = sorted(added)
        return (f"exported saved bytes; cancelled import wrote nothing; copied import {sorted(added)[0]}; "
                "a non-UTF-8 file name was exported with U+FFFD")

    def corner(self, window):
        _, right, _, bottom = window["frame"] or (0, 0, 0, 0)
        return (window["x"] + window["width"] + right - 3,
                window["y"] + window["height"] + bottom - 2)

    def case_layout(self):
        self.focus(EDITOR, "layout-start")
        start = self.window(EDITOR)
        x, y = self.corner(start)
        self.act("drag", "layout-larger", x=x, y=y, to_x=x + 120, to_y=y + 80)
        grown = self.window(EDITOR)
        check(grown["width"] >= start["width"] + 60 and grown["height"] >= start["height"] + 40,
              f"corner drag did not grow the window: {start} -> {grown}")
        x, y = self.corner(grown)
        self.act("drag", "layout-minimum", x=x, y=y, to_x=x - 900, to_y=y - 700)
        shrunk = self.editor_size()
        check(shrunk == MINIMUM, f"window not clamped to its minimum: {shrunk}")
        bars = [n for n in self.nodes() if n["role"] == "scroll bar" and n.get("showing")]
        check(len(bars) == 2, f"minimum size does not offer both scroll bars ({len(bars)})")
        x, y = self.corner(self.window(EDITOR))
        self.act("drag", "layout-restored", x=x, y=y, to_x=x + start["width"] - MINIMUM[0],
                 to_y=y + start["height"] - MINIMUM[1])
        editor = self.window(EDITOR)
        self.act("click", "layout-maximized", x=editor["x"] + 300, y=editor["y"] - 10, count=2)
        maximized = self.editor_size()
        width, height = self.shot["screen"]
        check(maximized[0] >= width - 10 and maximized[1] >= height - 60,
              f"title-bar double-click did not maximize: {maximized}")
        editor = self.window(EDITOR)
        self.act("click", "layout-unmaximized", x=editor["x"] + 300, y=editor["y"] - 10, count=2)
        restored = self.editor_size()
        check(restored[0] < maximized[0], f"second double-click did not restore: {restored}")
        self.key("ctrl+plus", "layout-scaled")
        scaled = self.editor_size()
        check(scaled[0] > restored[0] and scaled[1] > restored[1], f"Ctrl+Plus did not scale: {scaled}")
        self.key("ctrl+0", "layout-unscaled")
        unscaled = self.editor_size()
        check(abs(unscaled[0] - restored[0]) <= 2, f"Ctrl+0 did not restore the scale: {unscaled}")
        return (f"{start['width']}x{start['height']} -> {grown['width']}x{grown['height']} -> "
                f"{shrunk[0]}x{shrunk[1]} with scroll bars; maximized {maximized[0]}x{maximized[1]}; "
                f"scaled {scaled[0]}x{scaled[1]} and back")

    def refresh_daemon(self, label):
        self.click(self.find("button", "Daemon status"), label)
        return self.text(DAEMON)

    def case_daemon_reconnect(self):
        self.focus(EDITOR, "reconnect-focused")
        self.request("daemon_stop")
        self.capture("daemon-stopped")
        offline = self.until(lambda n: "not running" in (self.refresh_daemon("daemon-refresh-down") or ""),
                             "stopped LayerD was not reported", seconds=8)
        del offline
        self.key("ctrl+2", "reconnect-cooldowns")
        self.type_into("entry", "Duration (seconds)", "4.5", "offline-typed")
        outcome = self.save("saved-offline")
        entry, daemon = self.profile()
        check((entry["json"] or {})["cooldowns"][0]["durationMs"] == 4500, "offline save not durable")
        check(not daemon["reachable"], "daemon unexpectedly reachable")
        self.request("daemon_start")
        self.capture("daemon-started")
        status = self.refresh_daemon("daemon-refresh-up")
        check("not running" not in status and "LayerD" in status, f"restart not reported: {status!r}")
        self.type_into("entry", "Duration (seconds)", "5", "online-typed")
        online = self.save("saved-online")
        check("acknowledged" in online, f"reconnected Save not acknowledged: {online!r}")
        self.base_hash = self.profile()[0]["sha256"]
        return f"offline outcome {outcome!r}; after restart {online!r}"

    def case_store_failure_visible(self):
        self.request("block_profile", file=PROFILE)
        try:
            self.capture("store-blocked")
            self.key("ctrl+1", "failure-profile-page")
            self.type_into("entry", "Profile name", "Unsaved", "failure-typed")
            outcome = self.save("save-failure")
            entry, _ = self.profile()
            check(entry["kind"] == "directory", "store replaced the blocking directory")
        finally:
            self.request("restore_profile", file=PROFILE)
        entry, _ = self.profile()
        check(entry["sha256"] == self.base_hash, "original profile bytes changed")
        return f"save refused visibly ({outcome!r}); original profile restored byte-identical"


def main():
    parser = argparse.ArgumentParser(description="Scripted LayerE editor walkthrough")
    parser.add_argument("--session", required=True)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    walk = Walk(args.session)
    cases, failed = [], None
    for name in EXPECTED:
        record = {"id": name, "required": True, "expected": EXPECTED[name]}
        if failed:
            record.update(status="blocked", observed=f"not run after {failed} failed")
            cases.append(record)
            continue
        first, started = len(walk.shots), time.monotonic()
        try:
            observed = getattr(walk, "case_" + name.replace("-", "_"))()
            record.update(status="pass", observed=observed)
        except Exception as error:  # noqa: BLE001 -- every failure is recorded
            record.update(status="fail", observed=f"{type(error).__name__}: {error}")
            failed = name
        record.update(seconds=round(time.monotonic() - started, 3),
                      screenshots=walk.shots[first:])
        cases.append(record)
        print(f"{record['status']}: {name} — {record['observed']}", flush=True)
    report = {"cases": cases, "deferred": DEFERRED, "notes": walk.notes,
              "pass": failed is None}
    (args.out / "cases.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    return 0 if failed is None else 1


if __name__ == "__main__":
    sys.exit(main())
