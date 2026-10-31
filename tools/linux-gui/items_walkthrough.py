"""Typed-item cases for the owned X11 walkthrough fixture.

All edits use real input through the screenshot-citing bridge. No profile or
sidecar is written directly. Each item operation is undone before the next
case; the final case also undoes the temporary probe graph, restoring both
original durable hashes. Rendered screenshots remain evidence for human review.
"""
from copy import deepcopy
import re
from typing import Any

from automation_walkthrough import require

COUNT = "typed_items_count"
FLAG = "typed_items_flag"
PROBE = "typed_items_probe"
PANEL = "main"
ACTIVE = "Active duration (seconds; 0 = off)"
MAXIMUM = "Counter maximum (optional)"
STATUS = "Save / reload outcome"
ITEM_KINDS = {
    "cooldown": 0, "automation_timer": 1, "active_timer": 2,
    "counter": 3, "indicator": 4, "preset": 5,
}

EXPECTED = {
    "overlay-items-editor": "Reject invalid active duration without a write; edit its valid "
                            "value; add a labelled pips counter, reject and repair an invalid "
                            "maximum, cancel Edit, move up/down, then undo to the original profile.",
    "overlay-items-timer-preset": "Reject a 5 ms timer preset without changing either file; "
                                  "create a valid preset and timer item in one undo/redo transaction, "
                                  "then restore the original profile and automation bytes.",
    "overlay-items-simulator": "Show counter/indicator/active-timer items in the preview; step "
                               "a deterministic input changing the counter from 3 to 7 and the flag "
                               "from true to false without a durable write; restore both original hashes.",
}


def probe_graph(original, cooldown_id, source):
    """A temporary, explicitly typed fixture graph; never alter the caller's data."""
    result = deepcopy(original)
    variables = result.setdefault("variables", [])
    require(not any(v.get("id") in {COUNT, FLAG} for v in variables),
            "typed-item fixture variable IDs already exist")
    variables.extend([
        {"id": COUNT, "type": "int", "persistent": False, "default": 3},
        {"id": FLAG, "type": "bool", "persistent": False, "default": True},
    ])
    literal = lambda kind, value: {"op": "literal", "valueType": kind, "value": value}
    # Temporarily isolate the probe from the earlier rule's overlay overrides.
    # The original definition set is retained by the caller and by undo history.
    result["automations"] = [{
        "id": PROBE, "name": "Typed item preview probe", "enabled": True,
        "rules": [{
            "id": PROBE + "_input", "event": "inputPressed", "source": source,
            "condition": literal("bool", True),
            "actions": [
                {"type": "setVariable", "target": COUNT, "value": literal("int", 7)},
                {"type": "setVariable", "target": FLAG, "value": literal("bool", False)},
                {"type": "triggerCooldown", "target": cooldown_id},
            ],
        }],
    }]
    return result


def ordered_rows(nodes):
    """Only children of the named item list; never profile/automation siblings."""
    indices = [i for i, node in enumerate(nodes)
               if node["role"] == "list" and node["name"] == "Panel items (ordered)"]
    require(len(indices) == 1, "ordered item list is missing or ambiguous")
    index = indices[0]
    depth = nodes[index]["depth"]
    result = []
    for node in nodes[index + 1:]:
        if node["depth"] <= depth:
            break
        if node["role"] == "list item":
            result.append(node)
    return result


class ItemsWalk:
    def items_save(self: Any, label):
        before = self.text(STATUS)
        self.key("ctrl+s", label)
        dialog = self.dialog()
        if dialog and dialog[0]["name"] == "Upgrade profile":
            self.press("Upgrade", label + "-schema5-confirmed")
        after = self.until(lambda nodes: (value := self.find("text", STATUS, nodes).get("text", ""))
                           != before and value, f"no visible save outcome for {label}")
        require(not after.startswith("Not saved"), f"{label}: {after}")
        return after

    def items_panel(self: Any):
        profile = self.profile()[0]["json"]
        panel = next((p for p in profile.get("panels", []) if p.get("id") == PANEL), None)
        require(panel is not None, "typed-item fixture main panel is missing")
        return panel

    def items_choose_source(self: Any, source, label):
        _, choices = self.choice_items("Source")
        # add_choice prefixes every authored choice with its "[N] " ordinal.
        names = [re.sub(r"^\[\d+\] ", "", item["name"]) for item in choices]
        found = [i for i, name in enumerate(names)
                 if name == source or name.endswith(f" — {source}")]
        require(len(found) == 1, f"source {source!r} is missing/ambiguous: {choices}")
        self.automation_choice("Source", found[0], label)

    def items_open(self: Any, kind, source, label):
        self.click(self.find("button", "Add item"), label + "-open")
        self.automation_choice("Item type", ITEM_KINDS[kind], label + "-kind")
        self.items_choose_source(source, label + "-source")

    def items_label(self: Any, value, label):
        custom = self.find("check box", "Custom label")
        if not custom.get("checked", False):
            self.click(custom, label + "-custom")
        self.type_into("entry", "Item label", value, label)

    def items_apply(self: Any, label):
        self.press("Apply item", label)
        require(self.dialog() is None, f"item Apply was refused: {self.dialog()}")

    def items_undo(self: Any, count, label):
        for index in range(count):
            self.key("ctrl+z", label + f"-{index}")
        self.items_save(label + "-saved")

    def items_duration(self: Any, value, label):
        self.key("ctrl+2", label + "-cooldowns")
        self.click(self.find("list", "Cooldowns"), label + "-list")
        self.key("Home", label + "-first")
        self.type_into("entry", ACTIVE, value, label)

    def items_panels_page(self: Any, label):
        self.key("ctrl+4", label)
        self.click(self.find("list", "Panels"), label + "-list")
        self.key("Home", label + "-first")

    def case_overlay_items_editor(self: Any):
        self.capture("items-start")
        self.focus("LayerE", "items-focused")
        self.select_fixture("items-fixture")
        self.notes["items-original-profile"] = self.profile()[0]["sha256"]
        self.notes["items-original-module"] = self.automation_module()["sha256"]
        cooldown = self.profile()[0]["json"]["cooldowns"][0]["id"]
        self.notes["items-cooldown"] = cooldown
        self.key("ctrl+5", "items-automation")
        self.automation_choice("Automation view", 2, "items-json-view")
        graph = self.automation_graph()
        self.notes["items-original-graph"] = deepcopy(graph)
        prepared = probe_graph(graph, cooldown, self.notes["automation-source"])
        self.automation_type_json(prepared, "items-probe-json")
        self.click(self.find("button", "Validate / Apply JSON"), "items-probe-applied")
        self.items_save("items-probe-saved")
        self.notes["items-probe-profile"] = self.profile()[0]["sha256"]
        self.notes["items-probe-module"] = self.automation_module()["sha256"]

        self.items_duration("nan", "items-duration-invalid")
        before = self.profile()[0]["sha256"]
        self.key("ctrl+s", "items-duration-refused")
        require(self.text(STATUS).startswith("Not saved"), "invalid active duration was accepted")
        require(self.profile()[0]["sha256"] == before, "invalid duration changed durable bytes")
        self.items_duration("2.5", "items-duration-valid")
        self.items_save("items-duration-saved")
        require(self.profile()[0]["json"]["cooldowns"][0].get("activeDurationMs") == 2500,
                "active duration did not save as 2500 ms")
        self.items_panels_page("items-panels")
        require(len(ordered_rows(self.nodes())) == len(self.items_panel().get("cooldowns", [])),
                "legacy membership did not become non-mutating ordered rows")
        before = self.profile()[0]["sha256"]
        self.items_open("counter", COUNT, "items-counter")
        self.items_label("Stacks λ", "items-counter-label")
        self.automation_choice("Counter display", 1, "items-counter-pips")
        self.type_into("entry", MAXIMUM, "nan", "items-counter-invalid-max")
        self.press("Apply item", "items-counter-refused")
        require(self.dialog() is not None and self.profile()[0]["sha256"] == before,
                "invalid maximum closed the dialog or changed durable bytes")
        self.type_into("entry", MAXIMUM, "8", "items-counter-valid-max")
        self.items_apply("items-counter-applied")
        self.items_save("items-counter-saved")
        rows = self.items_panel()["items"]
        counter = rows[-1]
        require(counter.get("kind") == "counter" and counter.get("variable") == COUNT and
                counter.get("label") == "Stacks λ" and counter.get("display") == "pips" and
                counter.get("max") == 8, f"counter fields were lost: {counter}")
        ids = [row["id"] for row in rows]
        saved = self.profile()[0]["sha256"]
        self.click(self.find("button", "Edit item"), "items-edit-cancel-open")
        self.press("Cancel", "items-edit-cancelled")
        require(self.profile()[0]["sha256"] == saved, "cancelled item edit changed durable bytes")
        self.click(self.find("button", "Move up"), "items-moved-up")
        self.items_save("items-up-saved")
        require(self.items_panel()["items"][-2]["id"] == counter["id"], "Move up lost row order")
        self.click(self.find("button", "Move down"), "items-moved-down")
        self.items_save("items-down-saved")
        require([row["id"] for row in self.items_panel()["items"]] == ids,
                "Move down did not restore order")
        # Exactly four actions: duration, Add, Up, Down. Selection/Cancel add none.
        self.items_undo(4, "items-editor-restored")
        require(self.profile()[0]["sha256"] == self.notes["items-probe-profile"],
                "undo did not restore legacy profile bytes")
        return "active duration validated; labelled eight-pip counter; cancel/order/undo; legacy bytes restored"

    def case_overlay_items_timer_preset(self: Any):
        self.items_panels_page("items-preset-panels")
        before_profile = self.profile()[0]["sha256"]
        before_module = self.automation_module()["sha256"]
        self.items_open("preset", self.notes["items-cooldown"], "items-preset")
        self.type_into("entry", "Preset seconds", "0.005", "items-preset-invalid")
        self.press("Apply item", "items-preset-refused")
        require(self.dialog() is not None and self.profile()[0]["sha256"] == before_profile and
                self.automation_module()["sha256"] == before_module,
                "5 ms preset changed the profile/module or closed its dialog")
        self.type_into("entry", "Preset seconds", "2", "items-preset-repaired")
        self.items_apply("items-preset-applied")
        self.items_save("items-preset-saved")
        timer = self.items_panel()["items"][-1]
        require(timer.get("kind") == "timer" and timer.get("source") == "automation" and
                timer.get("timerId"), f"preset did not create an automation timer item: {timer}")
        after_profile = self.profile()[0]["sha256"]
        after_module = self.automation_module()["sha256"]
        require(after_profile != before_profile and after_module != before_module,
                "preset did not change both profile and module")
        self.items_undo(1, "items-preset-undone")
        require(self.profile()[0]["sha256"] == before_profile and
                self.automation_module()["sha256"] == before_module,
                "one Undo did not restore both files")
        self.key("ctrl+y", "items-preset-redone")
        self.items_save("items-preset-redo-saved")
        require(self.profile()[0]["sha256"] == after_profile and
                self.automation_module()["sha256"] == after_module,
                "one Redo did not restore both files")
        self.items_undo(1, "items-preset-restored")
        return "5 ms refused; valid timer preset and item save/undo/redo atomically; original files restored"

    def case_overlay_items_simulator(self: Any):
        self.items_duration("2.5", "items-sim-duration")
        self.items_save("items-sim-duration-saved")
        self.items_panels_page("items-sim-panels")
        self.items_open("counter", COUNT, "items-sim-counter")
        self.items_label("Count", "items-sim-counter-label")
        self.items_apply("items-sim-counter-applied")
        self.items_open("indicator", FLAG, "items-sim-indicator")
        self.items_label("Flag", "items-sim-indicator-label")
        self.type_into("entry", "Flash Hz (0 = off)", "2", "items-sim-flash-two-hz")
        self.items_apply("items-sim-indicator-applied")
        self.items_open("active_timer", self.notes["items-cooldown"], "items-sim-active-timer")
        self.items_label("Active", "items-sim-active-label")
        self.items_apply("items-sim-active-applied")
        self.items_save("items-sim-items-saved")
        self.capture("items-preview-defaults")
        before_profile = self.profile()[0]["sha256"]
        before_module = self.automation_module()["sha256"]
        self.key("ctrl+5", "items-sim-automation")
        self.automation_choice("Automation view", 1, "items-sim-view")
        self.click(self.find("button", "Reset"), "items-sim-reset")
        self.type_into("entry", "Breakpoint rule id", "", "items-sim-no-breakpoint")
        self.automation_choice("Synthetic event", 0, "items-sim-input")
        self.type_into("entry", "Event source", self.notes["automation-source"], "items-sim-source")
        self.type_into("entry", "Fields: name=JSON; ...", "", "items-sim-fields")
        self.type_into("entry", "Assertions: variable=JSON; ...",
                       COUNT + "=7;" + FLAG + "=false", "items-sim-assertions")
        self.click(self.find("button", "Add event"), "items-sim-event")
        self.click(self.find("button", "Step"), "items-sim-step")
        view = self.find("text", "Simulator state")
        require(not view.get("textTruncated", False), "simulator state exceeds the bounded probe")
        state = view.get("text", "")
        require("Events: 1/" in state and "Assertions: PASS" in state,
                f"typed-item simulation did not reach its declared values: {state}")
        self.capture("items-preview-simulated")
        require(self.profile()[0]["sha256"] == before_profile and
                self.automation_module()["sha256"] == before_module,
                "local simulation changed durable profile/module bytes")
        self.automation_choice("Automation view", 0, "items-sim-left")
        self.items_panels_page("items-sim-preview-restored")
        # Duration + counter + indicator + active timer; simulation adds no edit.
        self.items_undo(4, "items-sim-edits-undone")
        require(self.profile()[0]["sha256"] == self.notes["items-probe-profile"] and
                self.automation_module()["sha256"] == self.notes["items-probe-module"],
                "item cleanup did not restore the prepared fixture")
        self.items_undo(1, "items-probe-undone")
        require(self.profile()[0]["sha256"] == self.notes["items-original-profile"] and
                self.automation_module()["sha256"] == self.notes["items-original-module"],
                "typed-item cases left a durable fixture mutation")
        self.base_hash = self.profile()[0]["sha256"]
        return "defaults and deterministic simulation captured; counter=7, flag=false; both original hashes restored"
