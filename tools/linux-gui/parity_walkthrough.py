"""Audit-regression walkthroughs. Input only, in the disposable X11 session."""
from typing import Any

from automation_walkthrough import require

GROUPS = "Cooldown groups — LayerE"
EXPORT = "Select profiles to export — LayerE"
EXPECTED = {
    "cooldown-groups": "Create a shared pool, refuse invalid timing without a write, "
                       "save its charge/recovery controls, undo/redo, cancel an edit, "
                       "then remove it; unknown profile fields survive every save.",
    "export-profile-checklist": "Refuse an empty selection, select two nonadjacent saved "
                                "profiles through individual checkboxes, export a pack "
                                "through the chooser, and cancel without writing another file.",
}


def reviewed_destinations(dialog):
    """FLTK message boxes expose their message as a label name, not text."""
    if not dialog:
        return []
    for node in dialog[1]:
        message = node.get("name", "")
        if node.get("role") == "label" and message.startswith("Reviewed destinations:\n"):
            return [line for line in message.splitlines()[1:] if line]
    return []

class ParityWalk:
    def open_groups(self: Any, label):
        self.menu_item("alt+c", 4, label)
        self.until(lambda n: self.window_open(GROUPS, n), "Cooldown groups did not open")

    def case_cooldown_groups(self: Any):
        self.focus("LayerE", "groups-focused")
        self.select_fixture("groups-fixture")
        before = self.profile()[0]
        require(before["json"].get("futureProfile") == {
            "large": 18446744073709551615, "note": "opaque metadata"},
            "earlier editor saves lost the fixture's unknown profile data")
        self.open_groups("groups-open")
        self.press("Add group", "group-added")
        self.type_into("entry", "Group name", "Walkthrough pool", "group-named")
        self.automation_choice("Group mode", 1, "group-shared-pool")
        self.type_into("entry", "Group seconds", "nan", "group-invalid-seconds")
        self.press("Apply", "group-invalid-refused")
        require(self.window_open(GROUPS, self.nodes()) and
                self.profile()[0]["sha256"] == before["sha256"],
                "invalid group input changed the saved profile or closed the dialog")
        self.type_into("entry", "Group seconds", "2.5", "group-valid-seconds")
        self.type_into("entry", "Group charges", "3", "group-three-charges")
        self.automation_choice("Group recovery", 1, "group-sequential")
        self.automation_choice("Group retrigger", 1, "group-restart")
        self.press("Apply", "group-applied")
        self.wait_closed(GROUPS, "group-dialog-closed")
        self.autosaved("group-saved", lambda: len(self.profile()[0]["json"]["cooldownGroups"]) == 1)
        saved = self.profile()[0]["json"]
        group = saved["cooldownGroups"][0]
        require(group["name"] == "Walkthrough pool" and group["durationMs"] == 2500 and
                group["maxCharges"] == 3 and group["recovery"] == "sequential" and
                group["retrigger"] == "restart", f"group controls were lost: {group}")
        require(saved["futureProfile"] == before["json"]["futureProfile"] and
                saved["cooldowns"][0]["futureCooldown"] == {"keep": True},
                "group save lost opaque profile or cooldown data")
        self.key("ctrl+z", "group-undo")
        self.autosaved("group-undo-saved", lambda: not self.profile()[0]["json"]["cooldownGroups"])
        self.key("ctrl+shift+z", "group-redo")
        self.autosaved("group-redo-saved",
                       lambda: self.profile()[0]["json"]["cooldownGroups"] == [group])
        stable = self.profile()[0]["sha256"]
        self.open_groups("group-cancel-open")
        self.type_into("entry", "Group name", "Do not save", "group-cancel-name")
        self.press("Cancel", "group-cancelled")
        self.wait_closed(GROUPS, "group-cancel-closed")
        require(self.profile()[0]["sha256"] == stable, "Cancel saved a staged group edit")
        self.open_groups("group-remove-open")
        self.press("Remove group", "group-removed")
        self.press("Apply", "group-removal-applied")
        self.autosaved("group-removal-saved", lambda: not self.profile()[0]["json"]["cooldownGroups"])
        return "shared pool controls saved; invalid/cancel paths inert; exact undo/redo; opaque data retained"

    def case_export_profile_checklist(self: Any):
        self.focus("LayerE", "checklist-focused")
        self.select_fixture("checklist-fixture")
        self.key("ctrl+alt+shift+e", "checklist-open")
        nodes = self.until(lambda n: self.window_open(EXPORT, n) and n,
                           "Export pack did not open its profile checklist")
        dialog = self.dialog()
        require(dialog and dialog[0]["name"] == EXPORT, "the checklist is not the active dialog")
        items = [n for n in dialog[1] if n["role"] == "check box"]
        require(len(items) >= 3, "the checklist has fewer than three saved profiles")
        self.press("Select none", "checklist-none")
        self.press("Export", "checklist-empty-refused")
        require(self.window_open(EXPORT, self.nodes()), "an empty checklist closed")
        # Snapshot handles are not reused: every input grounds on a fresh tree.
        names = [items[0]["name"], items[-1]["name"]]
        for index, name in enumerate(names):
            self.click(self.find("check box", name), f"checklist-picked-{index}")
        self.press("Export", "checklist-confirmed")
        self.choose("Export", self.state()["exchange"] + "/selected.layercprofile", "checklist-pack")
        require("selected.layercprofile" in self.state()["exchangeFiles"], "selected pack was not written")
        self.notes["exportChecklistProfiles"] = names
        # Review the saved pack without committing: exactly two rows must
        # reach the summary rather than exporting every saved profile.
        self.key("ctrl+o", "checklist-review-open")
        self.choose("Import", self.state()["exchange"] + "/selected.layercprofile", "checklist-review")
        for index in range(2):
            self.until(lambda n: self.dialog(), "the pack did not offer the selected profile")
            self.press("Include", f"checklist-review-include-{index}")
            self.press("Copy", f"checklist-review-copy-{index}")
        destinations = self.until(lambda _: reviewed_destinations(self.dialog()),
                                  "the exported pack did not reach its reviewed summary")
        require(len(destinations) == 2 and all(" — copy" in row for row in destinations),
                f"the exported pack does not contain exactly two profiles: {destinations}")
        self.press("Cancel", "checklist-review-cancelled")
        before = self.state()["exchangeFiles"]
        self.key("ctrl+alt+shift+e", "checklist-cancel-open")
        self.until(lambda n: self.window_open(EXPORT, n), "the second checklist did not open")
        self.press("Cancel", "checklist-cancelled")
        self.wait_closed(EXPORT, "checklist-cancel-closed")
        require(self.state()["exchangeFiles"] == before, "Cancel changed the exchange files")
        return "empty selection refused; two nonadjacent profiles exported; Cancel wrote nothing"
