#!/usr/bin/env python3
"""Active-operation workload on the disposable representative editor store.

All mutations use the existing real-input bridge and production editor. The
continuous sampler is independently owned by the runner; this client only
records monotonic operation brackets and checked semantic outcomes.
"""
import argparse
import json
import sys
import time
from pathlib import Path

from active_resources import REQUIRED_WINDOWS
from walkthrough import Walk, check

EXPECTED = {
    "automation-rules-json": "Validated automation edits, undo/redo and durable save on 20 x 10 x 2.",
    "automation-simulator": "Real Play advances events; Pause and breakpoint stop playback.",
    "automation-live-trace": "Authenticated live recording receives real fixture input roots.",
    "visual-pixel-calibration": "Consented X11 capture produces and samples a real frame.",
    "visual-template-calibration": "PNG decode/capture, alpha-mask work, four references and durable hashes.",
    "active-template-matching": "Loaded template references are evaluated against live X11 frames.",
}
START = {"simulator-playing": "simulator-play", "trace-enabled": "trace-recording",
         "pixel-live": "x11-capture", "template-cancel-import-open": "template-processing",
         "template-match-live": "template-matching"}
END = {"simulator-paused": "simulator-play", "trace-departed": "trace-recording",
       "pixel-frozen": "x11-capture", "template-applied": "template-processing",
       "template-match-frozen": "template-matching"}


class ActiveWalk(Walk):
    def __init__(self, session):
        super().__init__(session)
        self.windows = {}

    def act(self, op, label, **args):
        if label in START:
            name = START[label]
            check(name not in self.windows, "duplicate active-operation start: " + name)
            self.windows[name] = {"start": time.monotonic(), "verified": False,
                                  "screenshots": [self.shot["relative"]]}
        result = super().act(op, label, **args)
        if label in END:
            name = END[label]
            check(name in self.windows and "end" not in self.windows[name],
                  "missing or duplicate active-operation start: " + name)
            self.windows[name].update(end=time.monotonic())
            self.windows[name]["screenshots"].append(self.shot["relative"])
        return result

    def select_fixture(self, label):
        # A 20-profile store can scroll the fixture out of the viewport. Never
        # translate an off-screen row index into a click on unrelated controls.
        profile = self.profile()[0]["json"]
        if self.text("Profile ID (read-only)") == profile["id"]:
            return
        matches = self.profile_rows(profile["name"])
        check(len(matches) == 1, "representative fixture row is missing or ambiguous")
        self.click(self.find("list", "Profiles"), label + "-list-focus")
        self.key("Home", label + "-list-home")
        for index in range(matches[0]):
            self.key("Down", label + f"-list-down-{index}")
        check(self.text("Profile ID (read-only)") == profile["id"],
              "keyboard selection did not open the representative fixture")

    def case_active_template_matching(self):
        self.calibrate("template-match-open")
        outcome = self.visual_start("template-match")
        check("template confidence" in outcome, "live capture did not evaluate the saved template")
        self.click(self.find("button", "Cancel calibration"), "template-match-closed")
        return outcome


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--session", required=True)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    walk = ActiveWalk(args.session)
    cases, failed, fixture = [], None, None
    try:
        walk.capture("active-start")
        profiles = walk.state()["profiles"]
        check(len(profiles) == 20 and all(isinstance(p.get("json"), dict)
              and len(p["json"].get("cooldowns", [])) == 10
              and len(p["json"].get("panels", [])) == 2 for p in profiles),
              "active workload did not load exactly 20 profiles x 10 cooldowns x 2 panels")
        fixture = {"profiles": 20, "cooldownsEach": 10, "panelsEach": 2}
    except Exception as error:  # noqa: BLE001 -- failure must survive teardown
        failed = f"representative fixture: {type(error).__name__}: {error}"
    for name, expected in EXPECTED.items():
        record = {"id": name, "required": True, "expected": expected}
        if failed:
            record.update(status="blocked", observed="not run after " + failed)
        else:
            first, started = len(walk.shots), time.monotonic()
            try:
                result = getattr(walk, "case_" + name.replace("-", "_"))()
                record.update(status="pass", observed=result)
            except Exception as error:  # noqa: BLE001 -- preserve the failing operation
                failed = name
                record.update(status="fail", observed=f"{type(error).__name__}: {error}")
            record.update(seconds=time.monotonic() - started, screenshots=walk.shots[first:])
        cases.append(record)
        print(f"{record['status']}: {name}: {record['observed']}", flush=True)
    if failed is None:
        if set(walk.windows) != set(REQUIRED_WINDOWS) or any("end" not in w for w in walk.windows.values()):
            failed = "active workload did not bracket every required running operation"
        else:
            for window in walk.windows.values():
                window["verified"] = True
    (args.out / "cases.json").write_text(json.dumps({"cases": cases, "deferred": {},
        "notes": walk.notes}, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    (args.out / "active-windows.json").write_text(json.dumps({"passed": failed is None,
        "error": failed, "fixture": fixture, "windows": walk.windows}, indent=2,
        ensure_ascii=False) + "\n", encoding="utf-8")
    return 0 if failed is None else 1


if __name__ == "__main__":
    sys.exit(main())
