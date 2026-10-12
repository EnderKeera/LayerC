"""Automation cases for the real-input, disposable X11 editor walkthrough.

Mixed into Walk; no document mutation, production socket or display access.
JSON is typed in bounded chunks through the existing fresh-screenshot bridge.
"""
import json
import re
import time
from typing import Any

EXPECTED = {
    "automation-rules-json": "Create a preset and variable, rename with Unicode, cancel and "
                             "confirm removal, reject malformed JSON without a write, "
                             "apply a valid rule property, undo/redo and save.",
    "automation-simulator": "Reject invalid synthetic fields, step and play/pause at a "
                            "selected speed, stop at a rule breakpoint, and check a "
                            "variable assertion without changing the saved profile.",
    "automation-live-trace": "Record real fixture input through authenticated LayerD, "
                             "save/load the collected trace, replay locally, stop on "
                             "view departure, and report a disconnected daemon.",
}

RECORD = "Record authenticated LayerD trace (saved profile only)"
ROW = 20  # Rules-and-actions row pitch at the default scale.


def require(condition, message):
    if not condition:
        raise AssertionError(message)


class AutomationWalk:
    def choice_items(self: Any, name):
        nodes = self.nodes()
        found = [i for i, n in enumerate(nodes) if n["role"] == "combo box" and n["name"] == name]
        require(len(found) == 1, f"combo box {name!r} is missing or ambiguous")
        depth, items = nodes[found[0]]["depth"], []
        for node in nodes[found[0] + 1:]:
            if node["depth"] <= depth:
                break
            if node["role"] == "menu item":
                items.append(node)
        return nodes[found[0]], items

    def automation_choice(self: Any, name, index, label):
        # FLTK pull-downs open on the current item and ignore Home, so move
        # relative to the exported selection and confirm the result.
        combo, items = self.choice_items(name)
        current = [i for i, item in enumerate(items) if item.get("selected")]
        require(len(current) == 1 and 0 <= index < len(items),
                f"combo box {name!r} exports no single selection among {len(items)} items")
        self.click(combo, label + "-open")
        delta = index - current[0]
        for step in range(abs(delta)):
            self.key("Down" if delta > 0 else "Up", label + f"-move-{step}")
        self.key("Return", label)
        _, items = self.choice_items(name)
        require(items[index].get("selected"), f"combo box {name!r} did not select item {index}")

    def automation_json(self: Any):
        entry = self.find("entry", "Advanced automation JSON")
        require(not entry.get("textTruncated", False), "automation JSON exceeds the bounded accessibility probe")
        return entry.get("text", "")

    def automation_graph(self: Any):
        try:
            value = json.loads(self.automation_json())
        except (TypeError, ValueError) as error:
            raise AssertionError("editor did not expose valid automation JSON") from error
        require(isinstance(value, dict), "editor automation JSON is not an object")
        return value

    def automation_type_json(self: Any, value, label):
        # Keep the bridge's 256-character action bound; every chunk is real
        # X input and cites a newly captured screenshot. No test-only UI API.
        text = value if isinstance(value, str) else json.dumps(value, ensure_ascii=False,
                                                              separators=(",", ":"))
        self.type_into("entry", "Advanced automation JSON", text[:200], label + "-first")
        for offset in range(200, len(text), 200):
            self.act("type", label + f"-{offset}", text=text[offset:offset + 200])

    def automation_module(self: Any):
        """The fixture's saved automation sidecar, read back from the store."""
        stem = self.notes.get("automation-stem", "fixture")
        module = self.state()["modules"].get(f"{stem}/automation.json")
        require(module is not None and isinstance(module.get("json"), dict),
                "saved automation sidecar is missing or unreadable")
        return module

    def automation_resources(self: Any, view):
        sample = self.request("editor_resources", view=view)
        require(sample["peakIdleUssBytes"] <= 48 * 1024 * 1024,
                f"advanced-view USS target exceeded: {sample}")
        require(sample["idleSeconds"] >= 60 and sample["idleCpuPercent"] <= 0.2,
                f"advanced-view idle CPU target exceeded: {sample}")
        self.notes.setdefault("automation-resources", {})[view] = sample

    def case_automation_rules_json(self: Any):
        self.focus("LayerE", "automation-focused")
        self.select_fixture("automation-fixture")
        self.key("ctrl+5", "automation-page")
        self.automation_choice("Starter preset", 1, "automation-input-preset")
        self.click(self.find("button", "New automation"), "automation-created")
        self.type_into("entry", "Automation name", "Native λ automation", "automation-renamed")
        self.automation_choice("Node palette", 5, "automation-variable-palette")
        self.click(self.find("button", "Add node"), "automation-variable-added")
        self.automation_choice("Starter preset", 0, "automation-blank-preset")
        self.click(self.find("button", "New automation"), "automation-temporary")
        self.click(self.find("button", "Remove automation"), "automation-remove-cancel-open")
        self.press("Cancel", "automation-remove-cancelled")
        self.click(self.find("button", "Remove automation"), "automation-remove-open")
        self.press("Remove", "automation-removed")
        self.automation_choice("Automation view", 2, "automation-json-view")
        original = self.automation_graph()
        require(len(original["automations"]) == 1, "automation removal did not preserve its sibling")
        require(original["automations"][0]["name"] == "Native λ automation", "name edit was lost")
        require(original.get("variables"), "palette variable was not added")
        self.notes["automation-variable"] = original["variables"][0]["id"]
        rule = original["automations"][0]["rules"][0]
        self.notes["automation-rule"] = rule["id"]
        self.notes["automation-source"] = rule.get("source", "")
        before = self.profile()[0]["sha256"]
        before_module = self.state()["modules"].get("fixture/automation.json")
        self.automation_type_json("{", "automation-json-invalid")
        self.click(self.find("button", "Validate / Apply JSON"), "automation-json-refused")
        failure = self.text("Save / reload outcome")
        require("Invalid" in failure and ("line" in failure.lower() or "character" in failure.lower()),
                f"JSON error did not include a location: {failure!r}")
        self.key("ctrl+s", "automation-invalid-save")
        refused = self.text("Save / reload outcome")
        require(refused != failure and refused.startswith("Not saved") and "Invalid" in refused,
                f"Save did not refuse pending invalid JSON: {refused!r}")
        require(self.profile()[0]["sha256"] == before, "invalid JSON changed the durable base profile")
        require(self.state()["modules"].get("fixture/automation.json") == before_module,
                "invalid JSON changed the durable automation sidecar")
        original["automations"][0]["rules"][0]["priority"] = 7
        self.automation_type_json(original, "automation-json-repaired")
        self.click(self.find("button", "Validate / Apply JSON"), "automation-json-applied")
        self.key("ctrl+z", "automation-json-undone")
        require(self.automation_graph()["automations"][0]["rules"][0].get("priority", 0) != 7,
                "automation edit was not undoable")
        self.key("ctrl+y", "automation-json-redone")
        require(self.automation_graph()["automations"][0]["rules"][0]["priority"] == 7,
                "automation redo did not restore the accepted edit")
        outcome = self.save("automation-saved")
        require("acknowledged" in outcome, f"automation save was not acknowledged: {outcome!r}")
        require(self.profile()[0]["json"].get("modules"), "save omitted the module reference")
        saved = self.automation_module()["json"]
        automations = saved.get("automations") or [{}]
        rules = automations[0].get("rules") or [{}]
        require(len(automations) == 1 and automations[0].get("name") == "Native λ automation"
                and rules[0].get("priority") == 7 and saved.get("variables"),
                f"saved automation graph lost an accepted edit: {saved!r}"[:400])
        self.base_hash = self.profile()[0]["sha256"]
        self.automation_resources("json")
        return f"preset, variable and removal; invalid JSON refused; priority undo/redo; {outcome}"

    def case_automation_simulator(self: Any):
        saved = self.profile()[0]["sha256"]
        saved_module = self.automation_module()["sha256"]
        self.automation_choice("Automation view", 0, "simulator-source-view")
        # Select the saved rule's row by its exported name; its property
        # text remains readable even when the compact Basic view clips it.
        # Rows report their list's bounds, so click by the row's position.
        prefix = f"Rule: {self.notes['automation-rule']} "
        nodes = self.nodes()
        lists = [i for i, n in enumerate(nodes) if n["role"] == "list" and n["name"] == "Rules and actions"]
        require(len(lists) == 1, "Rules and actions list is missing or ambiguous")
        x, y, width, _ = nodes[lists[0]]["extents"]
        rows = []
        for node in nodes[lists[0] + 1:]:
            if node["depth"] <= nodes[lists[0]]["depth"]:
                break
            if node["role"] == "list item":
                rows.append(node["name"])
        index = [i for i, name in enumerate(rows) if name.startswith(prefix)]
        require(len(index) == 1 and index[0] < 2, f"saved rule row is missing, ambiguous or clipped: {rows!r}")
        self.act("click", "simulator-rule-selected", x=x + width // 2, y=y + 4 + ROW * index[0] + ROW // 2)
        source = self.notes["automation-source"]
        shown = self.text("Source filter", role="entry")
        selected = [n["name"] for n in self.nodes() if n.get("selected") and n["name"].startswith("Rule: ")]
        require(shown == source, f"Basic rule selection showed source {shown!r}, saved {source!r}; "
                                 f"selected rows {selected!r}")
        self.automation_choice("Automation view", 1, "simulator-view")
        self.click(self.find("button", "Reset"), "simulator-reset")
        self.automation_choice("Synthetic event", 0, "simulator-input-kind")
        self.type_into("entry", "Event source", source, "simulator-source")
        self.type_into("entry", "Fields: name=JSON; ...", "untyped=1", "simulator-invalid-fields")
        before = self.text("Simulator state")
        self.click(self.find("button", "Add event"), "simulator-invalid-refused")
        require("Fields use" in self.text("Save / reload outcome"), "invalid synthetic field was accepted")
        require(self.text("Simulator state") == before, "invalid event changed the simulator")
        self.type_into("entry", "Fields: name=JSON; ...", "", "simulator-fields-cleared")
        for number in range(16):
            self.click(self.find("button", "Add event"), f"simulator-event-{number}")
        variable = self.notes["automation-variable"]
        self.type_into("entry", "Assertions: variable=JSON; ...", variable + "=false", "simulator-assertion")
        self.click(self.find("button", "Step"), "simulator-stepped")
        stepped = self.text("Simulator state")
        require("Events: 1/" in stepped, "Step did not consume exactly one event")
        self.automation_choice("Speed", 0, "simulator-quarter-speed")
        self.click(self.find("button", "Play"), "simulator-playing")
        self.click(self.find("button", "Pause"), "simulator-paused")
        paused = self.text("Simulator state")
        require(paused != stepped, "Play did not advance the simulator")
        # Quarter speed ticks every 400 ms; wait several ticks before comparing.
        time.sleep(1.5)
        self.capture("simulator-paused-later")
        require(self.text("Simulator state") == paused, "Pause left a playback timer running")
        self.click(self.find("button", "Reset"), "simulator-breakpoint-reset")
        self.type_into("entry", "Breakpoint rule id", self.notes["automation-rule"], "simulator-breakpoint")
        self.click(self.find("button", "Play"), "simulator-breakpoint-playing")
        self.until(lambda n: any(x["role"] == "button" and x["name"] == "Play" for x in n),
                   "rule breakpoint did not stop playback", seconds=8)
        state = self.text("Simulator state")
        require("Events: 2/" in state and "Assertions: PASS" in state, f"breakpoint/assertion failed: {state!r}")
        require(self.profile()[0]["sha256"] == saved and self.automation_module()["sha256"] == saved_module,
                "local simulation changed the saved profile")
        self.automation_resources("simulator")
        return f"invalid event refused; Step, Play/Pause and breakpoint; {state}"

    def case_automation_live_trace(self: Any):
        self.automation_choice("Automation view", 3, "trace-view")
        self.click(self.find("check box", RECORD), "trace-enabled")
        self.until(lambda n: "Recording" in self.find("text", "Live trace status", n).get("text", ""),
                   "LayerD trace did not connect")
        self.focus("LayerC Automation Fixture", "trace-game")
        for number in range(3):
            self.key("e", f"trace-input-{number}")
        self.focus("LayerE", "trace-editor")
        recorded = self.until(lambda n: (t := self.find("text", "Live trace status", n).get("text", ""))
                              and re.search(r"Roots: [1-9]", t) and t,
                              "no real input roots arrived from LayerD")
        counted = re.search(r"Roots: (\d+)", recorded)
        if counted is None:
            raise AssertionError(f"trace status reports no root count: {recorded!r}")
        roots = int(counted.group(1))
        # Leaving the view while recording must end the daemon subscription.
        self.automation_choice("Automation view", 1, "trace-departed")
        self.automation_choice("Automation view", 3, "trace-returned")
        stopped = self.text("Live trace status")
        require(not self.find("check box", RECORD).get("checked", False)
                and stopped == "Stopped. No daemon subscription.",
                f"leaving the view left live recording active: {stopped!r}")
        self.focus("LayerC Automation Fixture", "trace-game-after")
        self.key("e", "trace-input-after-stop")
        self.focus("LayerE", "trace-editor-after")
        time.sleep(1.0)
        require(self.text("Live trace status") == stopped, "stopped trace kept receiving roots")
        path = self.state()["exchange"] + "/recorded.layerctrace"
        self.click(self.find("button", "Save trace..."), "trace-save-open")
        self.press("Save", "trace-privacy-confirmed")
        self.choose("Save automation trace", path, "trace-saved")
        digest = self.state()["exchangeFiles"].get("recorded.layerctrace")
        require(digest, "trace chooser wrote no file")
        self.click(self.find("button", "Load trace..."), "trace-load-open")
        self.choose("Load automation trace", path, "trace-loaded")
        require("loaded" in self.text("Save / reload outcome").lower(), "trace file did not load")
        self.automation_choice("Automation view", 3, "trace-replay-view")
        self.click(self.find("button", "Replay collected trace in simulator"), "trace-replayed")
        replay = re.search(r"Events: 0/(\d+)", self.text("Simulator state"))
        require(replay is not None and int(replay.group(1)) >= roots,
                f"trace replay lost recorded roots (recorded {roots})")
        self.automation_choice("Automation view", 3, "trace-disconnect-view")
        self.request("daemon_stop")
        self.capture("trace-daemon-stopped")
        self.click(self.find("check box", RECORD), "trace-disconnect-enabled")
        fault = self.until(lambda n: "Unavailable" in (t := self.find("text", "Live trace status", n).get("text", ""))
                           and t, "disconnected trace did not show a reason")
        require(not self.find("check box", RECORD).get("checked", False),
                "fault left trace recording enabled")
        self.request("daemon_start")
        self.capture("trace-daemon-restarted")
        self.automation_choice("Automation view", 2, "trace-left")
        self.automation_resources("trace-stopped")
        self.base_hash = self.profile()[0]["sha256"]
        return f"{recorded}; file {digest}; local replay; disconnected reason {fault!r}"
