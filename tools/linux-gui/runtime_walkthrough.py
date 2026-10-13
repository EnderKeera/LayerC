"""Runtime Diagnostics, Help and cooldown command cases: real X input only."""
import re
import time
from typing import Any

from automation_walkthrough import require

DIAGNOSTICS = "Runtime diagnostics"
EXPECTED = {
    "runtime-diagnostics": "View > Runtime diagnostics shows the authenticated LayerD snapshot "
                           "(X11, capabilities, services, the fixture session); Refresh reports "
                           "a stopped LayerD and then its recovery; the open view stays idle.",
    "help-and-commands": "F1 lists the menu shortcuts, About names the version, F2 focuses the "
                         "cooldown name for a saved, undoable rename, and Duplicate cooldown "
                         "adds one unbound copy that autosaves and undoes exactly.",
}


class RuntimeWalk:
    def diagnostics_text(self: Any, nodes=None):
        return self.find("text", DIAGNOSTICS, nodes).get("text", "")

    def case_runtime_diagnostics(self: Any):
        self.focus("LayerE", "diagnostics-focused")
        self.key("alt+v", "diagnostics-view-menu")
        # Profile, Cooldowns, Bindings, Panels, Automation, Visual, Daemon status, Diagnostics.
        for step in range(8):
            self.key("Down", f"diagnostics-menu-{step}")
        self.key("Return", "diagnostics-opened")
        text = self.until(lambda n: (t := self.diagnostics_text(n)) and "Capabilities:" in t and t,
                          "Runtime Diagnostics did not show a detailed snapshot")
        require("Connection: LayerD connected and authenticated" in text and
                "Active profile: fixture" in text and "X11: connected" in text and
                "\n  fixture on window 0x" in text and "automation: active" in text,
                f"diagnostics snapshot is incomplete: {text[:600]!r}")
        version = re.search(r"^LayerD (\S+), diagnostics schema", text, re.MULTILINE)
        if version is None:
            raise AssertionError("diagnostics did not name the LayerD version")
        self.notes["version"] = version.group(1)
        self.window("Runtime Diagnostics")
        self.automation_resources("diagnostics-open")
        self.request("daemon_stop")
        try:
            self.capture("diagnostics-daemon-stopped")
            self.click(self.find("button", "Refresh"), "diagnostics-refresh-down")
            down = self.until(lambda n: (t := self.diagnostics_text(n)) and
                              "LayerD disconnected" in t and t,
                              "a stopped LayerD was not reported by Refresh")
            require("not running" in down, f"the stopped daemon reason is missing: {down!r}")
        finally:
            self.request("daemon_start")
        self.capture("diagnostics-daemon-started")
        # The restarted LayerD may still be binding its socket: refresh until it answers.
        for attempt in range(5):
            self.click(self.find("button", "Refresh"), f"diagnostics-refresh-up-{attempt}")
            try:
                self.until(lambda n: "Connection: LayerD connected and authenticated"
                           in self.diagnostics_text(n), "Refresh did not report the restarted LayerD")
                break
            except AssertionError:
                if attempt == 4:
                    raise
        self.click(self.find("button", "Close"), "diagnostics-closed")
        for attempt in range(15):
            if not any(w["name"].startswith("Runtime Diagnostics") for w in self.shot["windows"]):
                break
            time.sleep(0.2)
            self.capture(f"diagnostics-close-wait-{attempt}")
        else:
            raise AssertionError("Close left the diagnostics window open")
        first = text.splitlines()
        return f"{len(first)} lines; stop and restart reported by Refresh; closed"

    def case_help_and_commands(self: Any):
        self.focus("LayerE", "help-focused")
        self.key("F1", "help-shortcuts")
        shortcuts = self.until(lambda n: (t := self.find("text", "Keyboard shortcuts — LayerE", n)
                                          .get("text", "")) and t,
                               "F1 did not show the keyboard shortcuts")
        require("F2\tRename" in shortcuts and "Ctrl+S\tSave" in shortcuts and
                "F1\tKeyboard shortcuts" in shortcuts,
                f"shortcut help is incomplete: {shortcuts!r}"[:400])
        self.press("Close", "help-shortcuts-closed")
        self.key("alt+h", "help-menu")
        self.key("Down", "help-menu-shortcuts")
        self.key("Down", "help-menu-about")
        self.key("Return", "about-opened")
        about = self.until(lambda n: (t := self.find("text", "About LayerE", n).get("text", "")) and t,
                           "About did not open")
        require(about.startswith(f"LayerE {self.notes['version']} ") and
                "never accesses the network" in about,
                f"About text is unexpected: {about!r}"[:300])
        self.press("Close", "about-closed")
        self.select_fixture("rename-fixture")
        self.key("ctrl+2", "rename-cooldowns")
        original = self.profile()[0]["json"]
        self.key("F2", "rename-cooldown")
        self.until(lambda n: self.keyboard_focus("Cooldown name", n), "F2 did not focus the cooldown name")
        self.key("ctrl+a", "rename-select-all")
        self.act("type", "rename-typed", text="Renamed ability")
        self.autosaved("rename-saved",
                       lambda: self.profile()[0]["json"]["cooldowns"][0]["name"] == "Renamed ability")
        renamed = self.profile()[0]["json"]
        require(renamed["cooldowns"][0]["id"] == original["cooldowns"][0]["id"] and
                renamed["bindings"] == original["bindings"],
                "renaming changed the cooldown identity or its bindings")
        self.key("alt+e", "rename-edit-menu")
        self.key("Down", "rename-edit-undo")
        self.key("Return", "rename-undone")
        self.autosaved("rename-undo-saved", lambda: self.profile()[0]["json"] == original)
        self.key("ctrl+1", "rename-profile-page")
        self.key("F2", "rename-profile")
        self.until(lambda n: self.keyboard_focus("Profile name", n), "F2 did not focus the profile name")
        snapshot = self.profile()[0]["json"]
        before = snapshot["cooldowns"]
        self.key("ctrl+2", "duplicate-cooldowns")
        self.key("alt+c", "cooldown-menu")
        self.key("Down", "cooldown-menu-add")
        self.key("Down", "cooldown-menu-duplicate")
        self.key("Return", "cooldown-duplicated")
        copy_name = before[0]["name"] + " copy"
        self.autosaved("cooldown-duplicate-saved",
                       lambda: [c["name"] for c in self.profile()[0]["json"]["cooldowns"]][-1] == copy_name)
        saved = self.profile()[0]["json"]
        copy = saved["cooldowns"][-1]
        timing = ("durationMs", "maxCharges", "retrigger", "recovery")
        require(len(saved["cooldowns"]) == len(before) + 1 and saved["cooldowns"][:-1] == before and
                copy["id"] not in {c["id"] for c in before} and
                all(copy.get(key) == before[0].get(key) for key in timing) and
                saved["bindings"] == snapshot["bindings"],
                f"the duplicate is not one fresh, unbound timing copy: {copy!r}")
        # Edit > Undo, so no focused text field can take the keystroke.
        self.key("alt+e", "cooldown-edit-menu")
        self.key("Down", "cooldown-edit-undo")
        self.key("Return", "cooldown-duplicate-undone")
        self.autosaved("cooldown-undo-saved", lambda: self.profile()[0]["json"] == snapshot)
        self.base_hash = self.profile()[0]["sha256"]  # only after the exact restoration
        return f"shortcuts and About shown; F2 focus on both pages; {copy_name!r} saved and undone"
