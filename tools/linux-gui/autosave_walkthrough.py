"""Always-on autosave and leased live drafts, driven only by private X input."""
import time
from typing import Any

from automation_walkthrough import require

STATUS = "Save / reload outcome"
ACKNOWLEDGED = "LayerD acknowledged the reload request"
DAEMON = "Daemon status and capability faults"
EXPECTED = {
    "autosave-history-invalid": "Text edits autosave without Save, retain focus, and remain "
                                "undoable/redoable; invalid raw input never writes.",
    "live-draft-lease": "With a blocked durable write, a valid draft changes the real X11 "
                        "overlay only in memory; undo ends the lease and restores the overlay.",
    "autosave-legacy-consent": "Legacy edits pause autosave and live publication until an "
                               "explicit Save upgrade is approved; untouched data survives.",
    "autosave-offline-recovery": "Autosave remains durable while LayerD is stopped; a later "
                                 "edit reloads successfully after the owned daemon restarts.",
}


class AutosaveWalk:
    def autosaved(self: Any, label, durable, acknowledged=True):
        """Wait for the durable state itself, not only a (possibly older) status."""
        def settled(nodes):
            t = self.find("text", STATUS, nodes).get("text", "")
            ok = t.startswith("Autosaved") and (ACKNOWLEDGED in t if acknowledged else ACKNOWLEDGED not in t)
            return ok and durable() and t
        outcome = self.until(settled, f"no automatic durable Save for {label}", seconds=8)
        self.capture(label)
        return outcome

    def case_autosave_history_invalid(self: Any):
        self.focus("LayerE", "autosave-focused")
        self.select_fixture("autosave-fixture")
        self.key("ctrl+2", "autosave-cooldowns")
        duration = lambda: self.profile()[0]["json"]["cooldowns"][0]["durationMs"]  # noqa: E731
        before = duration()
        require(before != 6250, "fixture already has the autosave value")
        self.type_into("entry", "Duration (seconds)", "6.25", "autosave-typed")
        outcome = self.autosaved("autosave-durable", lambda: duration() == 6250)
        field = self.find("entry", "Duration (seconds)")
        require(field.get("focused") and field.get("text") == "6.25",
                f"autosave disturbed the focused text buffer: {field!r}")
        # A focused input owns its own text undo; leave it for document history.
        self.key("Tab", "autosave-field-left")
        self.key("ctrl+z", "autosave-undone")
        self.autosaved("autosave-undo-durable", lambda: duration() == before)
        self.key("ctrl+y", "autosave-redone")
        self.autosaved("autosave-redo-durable", lambda: duration() == 6250)
        digest = self.profile()[0]["sha256"]
        self.type_into("entry", "Duration (seconds)", "nan", "autosave-invalid")
        time.sleep(0.8)  # Beyond the documented 400 ms text debounce.
        require("Invalid" in self.text(STATUS), "invalid text did not retain its visible reason")
        require(self.profile()[0]["sha256"] == digest, "invalid raw input was autosaved")
        self.type_into("entry", "Duration (seconds)", "6.25", "autosave-invalid-repaired")
        time.sleep(0.8)
        require("Invalid" not in self.text(STATUS), "repaired input kept the invalid status")
        require(self.profile()[0]["sha256"] == digest,
                "repairing to the saved value rewrote the profile")
        self.base_hash = digest
        self.automation_resources("autosave-stopped")
        return f"{outcome}; focus retained; durable undo/redo; invalid text unchanged on disk"

    def case_live_draft_lease(self: Any):
        # Visual calibration shrinks the fixture; restore the overlay's reference size.
        game = self.window("LayerC Automation Fixture")
        self.act("set_geometry", "live-draft-fixture-size", window=game["id"],
                 x=60, y=80, width=1100, height=650)
        self.focus("LayerE", "live-draft-editor-first")
        self.key("ctrl+4", "live-draft-panels")
        original = self.profile()[0]["json"]["panels"][0]["anchorX"]
        self.focus("LayerC Automation Fixture", "live-draft-game-before")
        self.key("e", "live-draft-overlay-before")
        before = self.overlay_x()
        self.focus("LayerE", "live-draft-editor")
        self.request("block_profile", file="fixture.json")
        try:
            self.capture("live-draft-store-blocked")
            replacement = 0.15 if original >= 0.5 else 0.85
            self.type_into("entry", "X (0–1)", str(replacement), "live-draft-anchor")
            applied = self.until(
                lambda n: (t := self.find("text", DAEMON, n).get("text", ""))
                and "base-profile edits applied in memory, revision" in t and t,
                "real LayerD did not acknowledge the leased draft", seconds=8)
            failure = self.until(
                lambda n: (t := self.find("text", STATUS, n).get("text", ""))
                and t.startswith("Not autosaved") and t,
                "blocked autosave did not report failure", seconds=8)
            require(self.profile()[0]["kind"] == "directory", "autosave replaced the obstruction")
            self.focus("LayerC Automation Fixture", "live-draft-game-after")
            self.key("e", "live-draft-overlay-after")
            after = self.overlay_x()
            require(abs(after - before) > 100,
                    f"live draft did not move the real overlay: {before:.1f} -> {after:.1f}")
            self.focus("LayerE", "live-draft-resource-focused")
            self.automation_resources("live-draft-active")
            # 60 s is 24 lease periods: still applied means renewals worked,
            # so the restoration below can only come from Undo's explicit End.
            require("base-profile edits applied in memory" in self.text(DAEMON),
                    "the live draft was not active throughout the resource sample")
            self.focus("LayerC Automation Fixture", "live-draft-game-renewed")
            self.key("e", "live-draft-overlay-renewed")
            renewed = self.overlay_x()
            require(abs(renewed - after) < 40,
                    f"the leased draft lapsed during the sample: {after:.1f} -> {renewed:.1f}")
            self.focus("LayerE", "live-draft-editor-before-undo")
            self.key("Tab", "live-draft-field-left")
            self.key("ctrl+z", "live-draft-ended-by-undo")
            self.until(lambda n: "Live draft:" not in self.find("text", DAEMON, n).get("text", ""),
                       "undo left a live draft worker active")
            self.focus("LayerC Automation Fixture", "live-draft-restored-game")
            self.key("e", "live-draft-restored-overlay")
            restored = self.overlay_x()
            require(abs(restored - before) < 40,
                    f"ending the draft did not restore the committed overlay: {restored:.1f}")
        finally:
            self.request("restore_profile", file="fixture.json")
        self.capture("live-draft-original-restored")
        require(self.profile()[0]["sha256"] == self.base_hash,
                "an in-memory draft changed the durable profile bytes")
        self.focus("LayerE", "live-draft-editor-restored")
        return f"{applied}; {failure}; overlay {before:.1f} -> {after:.1f} -> {restored:.1f}"

    def case_autosave_legacy_consent(self: Any):
        self.row(self.row_of("Legacy autosave fixture"), "legacy-selected")
        self.key("ctrl+1", "legacy-profile-page")
        before = self.store()["y-legacy.json"]["sha256"]
        legacy = self.store()["y-legacy.json"]["json"]
        self.type_into("entry", "Profile name", "Legacy renamed", "legacy-edited")
        paused = self.until(
            lambda n: (t := self.find("text", STATUS, n).get("text", ""))
            and t.startswith("Autosave paused") and t,
            "legacy autosave did not pause for consent")
        require(self.store()["y-legacy.json"]["sha256"] == before,
                "legacy format was upgraded without consent")
        require("base-profile edits applied" not in self.text(DAEMON),
                "legacy draft was published without consent")
        self.key("ctrl+s", "legacy-upgrade-open")
        self.until(lambda _: self.dialog(), "explicit Save did not ask for upgrade consent")
        self.press("Cancel", "legacy-upgrade-cancel")
        require(self.store()["y-legacy.json"]["sha256"] == before,
                "cancelled upgrade changed the profile")
        self.key("ctrl+s", "legacy-upgrade-again")
        self.until(lambda _: self.dialog(), "upgrade confirmation did not reopen")
        self.press("Upgrade", "legacy-upgrade-approved")
        self.until(lambda n: ACKNOWLEDGED in self.find("text", STATUS, n).get("text", ""),
                   "approved upgrade was not saved and reloaded")
        saved = self.store()["y-legacy.json"]["json"]
        require(saved["schemaVersion"] == 4 and saved["name"] == "Legacy renamed",
                f"approved upgrade lost an edit: {saved!r}")
        def untouched(profile):  # Serializers may add defaults; compare the meaning.
            return (profile["id"], profile["enabled"], profile["game"].get("executableName"),
                    [(c["id"], c["name"], c["durationMs"], c["maxCharges"]) for c in profile["cooldowns"]],
                    [(b["cooldownId"], [(t["device"], t["code"]) for t in b["chord"]])
                     for b in profile["bindings"]],
                    [(p["id"], p["anchorX"], p["anchorY"], p["cooldowns"]) for p in profile["panels"]])
        require(untouched(saved) == untouched(legacy),
                f"upgrade changed data the edit did not touch: {untouched(legacy)!r} -> {untouched(saved)!r}"[:600])
        self.type_into("entry", "Profile name", "Legacy autosave fixture", "legacy-autosave-after")
        self.autosaved("legacy-autosave-resumed",
                       lambda: self.store()["y-legacy.json"]["json"]["name"] == "Legacy autosave fixture")
        self.select_fixture("legacy-return-to-fixture")
        return f"{paused}; Cancel preserved bytes; Upgrade kept untouched data; autosave resumed"

    def case_autosave_offline_recovery(self: Any):
        self.key("ctrl+2", "autosave-offline-cooldowns")
        self.request("daemon_stop")
        try:
            self.capture("autosave-daemon-stopped")
            self.type_into("entry", "Duration (seconds)", "7.25", "autosave-offline-typed")
            offline = self.autosaved(
                "autosave-offline-durable",
                lambda: self.profile()[0]["json"]["cooldowns"][0]["durationMs"] == 7250,
                acknowledged=False)
            require(not self.state()["daemon"]["reachable"], "owned daemon did not stop")

        finally:
            self.request("daemon_start")
        self.capture("autosave-daemon-restarted")
        self.type_into("entry", "Duration (seconds)", "8.25", "autosave-recovery-typed")
        online = self.autosaved(
            "autosave-recovery-durable",
            lambda: self.profile()[0]["json"]["cooldowns"][0]["durationMs"] == 8250)
        self.base_hash = self.profile()[0]["sha256"]
        return f"offline {offline}; reconnected {online}"
