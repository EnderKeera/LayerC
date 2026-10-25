#!/usr/bin/env python3
"""Real-input boundary operations on one frozen, disposable selected document.

The runner owns the editor and continuous sampler. This client records operation
brackets and semantic outcomes only; a passing client is not a resource verdict.
"""
import argparse
import json
import os
import shutil
import time
from pathlib import Path

from limit_inputs import command, digest, load_case, write_json
from walkthrough import Walk, check

EXPECTED = {
    "boundary-open": "Select the exact frozen valid profile/module and expose its editor view.",
    "boundary-operation": "Exercise bounded history/save or trace import; preserve authored data on refusal.",
    "boundary-recovery": "A valid save or trace import succeeds after the boundary operation.",
}
REQUIRED_WINDOWS = tuple(EXPECTED)


def json_kind(value):
    """JSON has one number type: a canonical writer may emit 0.0 as 0."""
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return "number"
    return type(value)


def contains_authored(expected, actual):
    """Known defaults may be added, but no original value/member may be lost.

    Numbers compare by exact value regardless of int/float spelling; booleans,
    strings, arrays and objects never match a different kind.
    """
    if json_kind(expected) != json_kind(actual):
        return False
    if isinstance(expected, dict):
        return all(key in actual and contains_authored(value, actual[key])
                   for key, value in expected.items())
    if isinstance(expected, list):
        return len(expected) == len(actual) and all(contains_authored(a, b)
            for a, b in zip(expected, actual, strict=True))
    return expected == actual


def verify_automation_preserved(out, probe):
    """Compare production typed values, not plain versus tagged JSON spelling."""
    probe_sha = digest(probe)
    retained = out / "module-preservation-probe"
    check(not retained.exists(), "module comparator artifact already exists")
    shutil.copy2(probe, retained)
    check(digest(retained) == probe_sha, "module comparator changed during capture")
    code, verdict = command([retained, "--compare-automation", out / "profile-before.json",
                             out / "module-before.json", out / "module-after.json"],
                            out / "module-preservation")
    check(digest(probe) == digest(retained) == probe_sha,
          "module comparator changed during comparison")
    receipt = {"probeSha256": probe_sha, "beforeSha256": digest(out / "module-before.json"),
               "afterSha256": digest(out / "module-after.json"),
               "exit": code, "verdict": verdict}
    write_json(out / "module-preservation.json", receipt)
    check(code == 0 and verdict == {"accepted": True, "equivalent": True},
          "successful base edit changed authored automation values")
    return receipt


class LimitsWalk(Walk):
    def __init__(self, session, selection, out):
        super().__init__(session)
        self.out = out
        self.selection = selection
        self.windows = {}
        self.original: dict = {}
        self.original_module: dict | None = None
        self.operation = None

    def bracket(self, name, function):
        check(name not in self.windows, "duplicate boundary operation")
        before = self.capture(name + "-before")["relative"]
        window = {"start": time.monotonic(), "verified": False, "screenshots": [before]}
        self.windows[name] = window
        result = function()
        after = self.capture(name + "-after")["relative"]
        window.update(end=time.monotonic(), verified=True, screenshots=[before, after])
        return result

    def retain_module(self, module, phase):
        check(os.environ.get("GUI_MODE") == "limits", "module capture requires the private limits runner")
        kind = self.selection["case"]["kind"]
        source = Path(os.environ["XDG_DATA_HOME"]) / "layerc/profiles/fixture" / (kind + ".json")
        target = self.out / ("module-" + phase + ".json")
        check(not target.exists(), "module evidence already exists")
        target.write_bytes(source.read_bytes())
        check(digest(target) == module["sha256"], "module changed during evidence capture")

    def case_boundary_open(self):
        self.focus("LayerE", "boundary-focused")
        self.row(0, "boundary-selected")
        check(self.text("Profile ID (read-only)") == self.selection["profileId"],
              "selection did not open the frozen boundary profile")
        self.original = self.profile()[0]
        check(self.original["sha256"] == self.selection["profileSha256"]
              and isinstance(self.original.get("json"), dict), "boundary base changed before opening")
        kind = self.selection["case"]["kind"]
        if kind == "automation":
            self.original_module = self.automation_module()
            self.key("ctrl+5", "boundary-automation")
            self.automation_choice("Automation view", 2, "boundary-automation-json")
        elif kind == "visual":
            self.original_module = self.visual_module()
            self.key("ctrl+6", "boundary-visual")
        elif kind == "trace":
            self.key("ctrl+5", "boundary-trace")
            self.automation_choice("Automation view", 1, "boundary-simulator")
        if self.original_module is not None:
            check(self.original_module["sha256"] == self.selection["case"]["sha256"],
                  "editor did not load the exact frozen sidecar")
            self.retain_module(self.original_module, "before")
        return {"profileSha256": self.original["sha256"], "kind": kind}

    def load_trace(self, path, label, accepted=True):
        self.click(self.find("button", "Load trace..."), label + "-chooser")
        self.choose("Load automation trace", str(path), label)
        # The recovery may reload exactly the same trace and therefore display
        # the same success text; requiring a changed string would falsely fail.
        prefix = "Trace loaded as simulator roots." if accepted else "Trace import resource limit exceeded"
        return self.until(lambda nodes: (value := self.find("text", "Save / reload outcome", nodes)
                          .get("text", "")).startswith(prefix) and value,
                          "trace import did not expose its expected visible outcome", seconds=30)

    def case_boundary_operation(self):
        record = self.selection["case"]
        if record["kind"] == "trace":
            previous = self.text("Simulator state")
            accepted = record["editorOperationAccepted"]
            outcome = self.load_trace(self.selection["inputPath"], "boundary-trace-loaded", accepted)
            if accepted:
                check("loaded" in outcome.lower(), "valid bounded trace was not loaded: " + outcome)
                check("Events: 0/" + str(record["roots"]) in self.text("Simulator state"),
                      "import did not retain the expected root-event count")
            else:
                check("Trace import resource limit exceeded (131072 JSON values)" in outcome,
                      "excessive allocation did not expose the declared refusal reason: " + outcome)
                check(self.text("Simulator state") == previous,
                      "resource refusal replaced the current simulator")
            check(self.profile()[0]["sha256"] == self.original["sha256"],
                  "trace import modified the authored profile")
            self.operation = {"accepted": accepted, "outcome": outcome,
                              "sourceSha256": digest(self.selection["inputPath"])}
        else:
            self.key("ctrl+1", "boundary-profile-form")
            capacity = self.selection["compiledLimits"]["historyCapacity"]
            check(type(capacity) is int and capacity == 50, "unexpected reviewed history capacity")
            for index in range(capacity):
                # Same UTF-8 byte length as 'Limit document': grow history, not input size.
                self.type_into("entry", "Profile name", f"Bound {index:08d}",
                               f"boundary-name-{index}")
                self.key("Tab", f"boundary-edit-ended-{index}")
            before = self.text("Save / reload outcome")
            self.key("ctrl+s", "boundary-save")
            outcome = self.until(lambda nodes: (value := self.find("text", "Save / reload outcome", nodes)
                                 .get("text", "")) != before and value,
                                 "boundary save produced no visible outcome", seconds=30)
            saved = self.profile()[0]
            accepted = "acknowledged" in outcome
            preservation = None
            if accepted:
                expected = dict(self.original["json"], name=f"Bound {capacity - 1:08d}")
                check(contains_authored(expected, saved.get("json")),
                      "successful save lost authored/unknown profile values")
                private_base = Path(os.environ["XDG_CONFIG_HOME"]) / "layerc/profiles/fixture.json"
                check(private_base.stat().st_size <= self.selection["compiledLimits"]["profileBytes"],
                      "successful save produced an unreadable oversized base profile")
                if self.original_module is not None:
                    current = (self.automation_module() if record["kind"] == "automation"
                               else self.visual_module())
                    if not isinstance(current, dict):
                        raise ValueError("saved module disappeared")
                    self.retain_module(current, "after")
                    if record["kind"] == "automation":
                        preservation = verify_automation_preserved(
                            self.out, Path(os.environ["GUI_BUILD"]) / "layerc_editor_resource_probe")
                    else:
                        check(contains_authored(self.original_module["json"], current.get("json")),
                              "successful base edit lost authored sidecar values")
            else:
                check("resource limit" in outcome.lower(),
                      "boundary save failed without a declared resource reason: " + outcome)
                check(saved["sha256"] == self.original["sha256"],
                      "resource refusal changed the durable base")
                if self.original_module is not None:
                    current = (self.automation_module() if record["kind"] == "automation"
                               else self.visual_module())
                    if not isinstance(current, dict):
                        raise ValueError("refused operation lost its module")
                    self.retain_module(current, "after")
                    check(current["sha256"] == self.original_module["sha256"],
                          "resource refusal changed the durable sidecar")
            self.operation = {"accepted": accepted, "outcome": outcome,
                              "historyEdits": capacity, "durableProfileSha256": saved["sha256"]}
            if preservation is not None:
                self.operation["sidecarPreservation"] = preservation
        check(digest(self.selection["inputPath"]) == record["sha256"],
              "boundary operation changed the frozen authored input")
        return self.operation

    def case_boundary_recovery(self):
        if self.selection["case"]["kind"] == "trace":
            inputs = Path(self.selection["inputPath"]).parent.parent
            _, record, source = load_case(inputs, "traces:trace-entries-limit")
            outcome = self.load_trace(source, "boundary-recovery-trace")
            check("loaded" in outcome.lower() and "Events: 0/" + str(record["roots"])
                  in self.text("Simulator state"), "valid trace did not recover after boundary import")
            check(self.profile()[0]["sha256"] == self.original["sha256"],
                  "trace recovery changed the saved profile")
        else:
            self.key("ctrl+n", "boundary-recovery-new")
            if self.dialog() is not None:
                self.press("Discard", "boundary-recovery-discard")
            self.key("ctrl+1", "boundary-recovery-form")
            self.type_into("entry", "Profile name", "Resource recovery", "boundary-recovery-name")
            self.type_into("entry", "Executable name", "lctest.exe", "boundary-recovery-game")
            outcome = self.save("boundary-recovery-saved")
            check("acknowledged" in outcome, "valid save did not recover after boundary operation: " + outcome)
            check(any(isinstance(row.get("json"), dict) and row["json"].get("name") == "Resource recovery"
                      for row in self.state()["profiles"]), "recovery save did not produce a valid profile")
        return {"outcome": outcome, "recovered": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--session", required=True)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    selection = json.loads((args.out / "limit-case.json").read_text(encoding="utf-8"))
    walk = LimitsWalk(args.session, selection, args.out)
    cases, failed = [], None
    for name, expected in EXPECTED.items():
        row = {"id": name, "required": True, "expected": expected}
        if failed:
            row.update(status="blocked", observed="not run after " + failed)
        else:
            started = time.monotonic()
            try:
                observed = walk.bracket(name, getattr(walk, "case_" + name.replace("-", "_")))
                row.update(status="pass", observed=observed)
            except Exception as error:  # noqa: BLE001 -- evidence must survive cleanup
                failed = name
                row.update(status="fail", observed=f"{type(error).__name__}: {error}")
            row["seconds"] = time.monotonic() - started
        cases.append(row)
        print(f"{row['status']}: {name}: {row['observed']}", flush=True)
    fixture = {"boundaryCase": selection["case"]["caseId"],
               "inputsManifestSha256": selection["inputsManifestSha256"]}
    (args.out / "cases.json").write_text(json.dumps({"cases": cases, "deferred": {}}, indent=2) + "\n")
    (args.out / "limit-windows.json").write_text(json.dumps({"passed": failed is None,
        "fixture": fixture, "windows": walk.windows, "operation": walk.operation,
        "error": failed}, indent=2) + "\n")
    return 0 if failed is None else 1


if __name__ == "__main__":
    raise SystemExit(main())
