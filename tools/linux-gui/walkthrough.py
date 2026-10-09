#!/usr/bin/env python3
"""Scripted walkthrough of the Linux LayerE X11 slice, driven through the bridge.

Uses exactly the protocol an interactive agent uses: capture, cite the fresh
screenshot for one action, capture again, then verify pixels, the durable
profile and daemon state. Writes cases.json; exits nonzero unless every
required case passes. A failure leaves later cases blocked, never passed.
"""
import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bridge import call  # noqa: E402

EDITOR = "LayerE — Linux X11 vertical slice"
GAME = "LayerC Automation Fixture"
CYAN = [80, 186, 200]
FIELDS = {"name": (400, 55), "duration": (700, 165), "panel_x": (400, 235)}
STATUS = (15, 495, 870, 45)  # Save-outcome text box, client coordinates.
MINIMUM = (900, 765)
PROFILE = "fixture.json"

EXPECTED = {
    "bridge-guards": "Bad token, out-of-screen input, stale screenshot and unknown "
                     "operations are rejected and logged without acting.",
    "startup": "Editor and game fixture are visible; the profile is a file; LayerD answers.",
    "invalid-no-write": "Duration 'nan' then Ctrl+S leaves the durable profile "
                        "byte-identical and changes the visible status.",
    "edit-commit-reload-overlay": "Duration 3.25 s and panel X 0.75 are saved durably, "
                                  "LayerD reloads, and the live overlay moves right.",
    "resize-drag": "A frame-corner drag grows the window; dragging below the "
                   "minimum clamps it to 900x765.",
    "offline-save": "With LayerD stopped, Save still commits durably and reports "
                    "a different, visible outcome.",
    "store-failure-visible": "When the store cannot replace the profile, Save fails "
                             "visibly and the original profile bytes survive.",
}
DEFERRED = {
    "menus": "the slice has no menus or context menus (Change 5)",
    "profiles-crud": "create/rename/delete/undo arrive in Change 5",
    "import-export": "bundle import/export arrives in Change 5",
    "keyboard-accessibility": "the production AT-SPI bridge arrives in Change 5",
    "maximize-scroll-font-scaling": "layout coverage for the full editor (Change 5)",
    "daemon-reconnect": "reconnect needs a runner-restartable daemon (Change 5)",
}


class Failure(Exception):
    pass


def check(condition, message):
    if not condition:
        raise Failure(message)


class Walk:
    def __init__(self, session):
        self.session = session
        self.shot = {}
        self.shots = []
        self.notes = {}
        self.base_hash = None

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

    def window(self, title):
        for window in self.shot["windows"]:
            if window["name"] == title:
                return window
        raise Failure(f"window not visible: {title}")

    def act(self, op, label, **args):
        self.request(op, screenshot=self.shot["screenshot"], **args)
        return self.capture(label)

    def field(self, name, text, label):
        editor = self.window(EDITOR)
        x, y = FIELDS[name]
        return self.act("set_text", label, x=editor["x"] + x, y=editor["y"] + y, text=text)

    def focus(self, title, label):
        return self.act("activate", label, window=self.window(title)["id"])

    def region(self, x, y, w, h):
        width, height = self.shot["screen"]
        x, y = max(0, x), max(0, y)
        return [x, y, min(w, width - x), min(h, height - y)]

    def status_digest(self):
        editor = self.window(EDITOR)
        x, y, w, h = STATUS
        return self.request("inspect", screenshot=self.shot["screenshot"],
                            region=self.region(editor["x"] + x, editor["y"] + y, w, h))["sha256"]

    def save(self, label):
        """Ctrl+S; the status text must change across the Save itself, not
        merely differ from an earlier editing state."""
        before = self.status_digest()
        self.act("key", label, combo="ctrl+s")
        after = self.status_digest()
        check(after != before, f"no visible outcome for {label}")
        return after

    def profile(self):
        state = self.request("state")
        entry = next((p for p in state["profiles"] if p["name"] == PROFILE), None)
        if entry is None:
            raise Failure("fixture profile entry is missing")
        return entry, state["daemon"]

    def overlay_x(self):
        game = self.window(GAME)
        found = self.request("inspect", screenshot=self.shot["screenshot"], rgb=CYAN,
                             region=self.region(game["x"], game["y"], game["width"], game["height"]))
        check(found["count"] > 100, f"overlay not visible over the game ({found['count']} px)")
        return found["meanX"]

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
        editor = self.window(EDITOR)
        self.window(GAME)
        check(editor["width"] >= MINIMUM[0] and editor["height"] >= MINIMUM[1],
              f"editor smaller than its minimum: {editor}")
        entry, daemon = self.profile()
        check(entry["kind"] == "file" and daemon["reachable"], "store or daemon not ready")
        self.base_hash = entry["sha256"]
        tree = self.request("tree")
        self.notes["accessibility"] = {k: tree.get(k) for k in ("available", "reason", "applications")}
        return f"editor {editor['width']}x{editor['height']}; accessibility available={tree.get('available')}"

    def case_invalid_no_write(self):
        self.focus(EDITOR, "editor-focused")
        self.field("duration", "nan", "invalid-typed")
        self.save("invalid-save")
        entry, _ = self.profile()
        check(entry["sha256"] == self.base_hash, "invalid input changed the durable profile")
        return "profile bytes unchanged; status changed across the refused Save"

    def case_edit_commit_reload_overlay(self):
        self.focus(GAME, "game-focused")
        self.act("key", "overlay-before", combo="e")
        before = self.overlay_x()
        self.focus(EDITOR, "editor-refocused")
        self.field("duration", "3.25", "duration-typed")
        self.field("panel_x", "0.75", "panel-typed")
        self.save("saved")
        entry, daemon = self.profile()
        profile = entry["json"] or {}
        check(profile["cooldowns"][0]["durationMs"] == 3250, "duration not saved")
        check(profile["panels"][0]["anchorX"] == 0.75, "panel X not saved")
        check(daemon["reachable"], "daemon stopped answering after reload")
        self.focus(GAME, "game-refocused")
        self.act("key", "overlay-after", combo="e")
        after = self.overlay_x()
        check(after - before > 250, f"overlay did not move right: {before:.1f} -> {after:.1f}")
        return f"durationMs 3250, anchorX 0.75; overlay meanX {before:.1f} -> {after:.1f}"

    def corner(self, window):
        left, right, top, bottom = window["frame"] or (0, 0, 0, 0)
        del left, top
        return (window["x"] + window["width"] + right - 3,
                window["y"] + window["height"] + bottom - 2)

    def case_resize_drag(self):
        self.focus(EDITOR, "resize-start")
        start = self.window(EDITOR)
        x, y = self.corner(start)
        self.act("drag", "resized-larger", x=x, y=y, to_x=x + 120, to_y=y + 80)
        grown = self.window(EDITOR)
        check(grown["width"] >= start["width"] + 60 and grown["height"] >= start["height"] + 40,
              f"corner drag did not grow the window: {start} -> {grown}")
        x, y = self.corner(grown)
        self.act("drag", "resized-minimum", x=x, y=y, to_x=x - 400, to_y=y - 300)
        shrunk = self.window(EDITOR)
        check((shrunk["width"], shrunk["height"]) == MINIMUM,
              f"window not clamped to its minimum: {shrunk}")
        return (f"{start['width']}x{start['height']} -> {grown['width']}x{grown['height']}"
                f" -> {shrunk['width']}x{shrunk['height']}")

    def case_offline_save(self):
        self.request("daemon_stop")
        self.capture("daemon-stopped")
        self.field("duration", "4.5", "offline-typed")
        self.save("saved-offline")
        entry, daemon = self.profile()
        check((entry["json"] or {})["cooldowns"][0]["durationMs"] == 4500, "offline save not durable")
        check(not daemon["reachable"], "daemon unexpectedly reachable")
        self.base_hash = entry["sha256"]
        return "durationMs 4500 committed while LayerD was stopped"

    def case_store_failure_visible(self):
        self.request("block_profile", file=PROFILE)
        try:
            self.capture("store-blocked")
            self.field("name", "Unsaved", "failure-typed")
            self.save("save-failure")
            entry, _ = self.profile()
            check(entry["kind"] == "directory", "store replaced the blocking directory")
        finally:
            self.request("restore_profile", file=PROFILE)
        entry, _ = self.profile()
        check(entry["sha256"] == self.base_hash, "original profile bytes changed")
        return "save refused visibly; original profile restored byte-identical"


def main():
    parser = argparse.ArgumentParser(description="Scripted LayerE slice walkthrough")
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
    (args.out / "cases.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0 if failed is None else 1


if __name__ == "__main__":
    sys.exit(main())
