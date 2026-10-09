#!/usr/bin/env python3
"""Repeated profile-switch/dialog cycles on the representative store.

Drives LayerE through the bridge with real input: each cycle selects a profile
row, opens the Delete confirmation and cancels it with Escape. After a warm-up
it samples editor USS; the approved target is a final USS within 2 MiB of the
warmed baseline with no continuing upward trend. Writes cycles.json and exits
nonzero unless every cycle ran and the target holds.
"""
import argparse
import json
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sample import memory  # noqa: E402
from walkthrough import EDITOR, Walk, check  # noqa: E402

MIB = 1024 * 1024
WARMUP = 10
CYCLES = 100
CHECKPOINT = 10
ROWS = 15             # Rows visible without scrolling at the default size.
PROFILES = 20
DRIFT_LIMIT = 2 * MIB
TREND_FLOOR = MIB // 4  # A strictly rising tail must also be material.


def settled_uss(pid, seconds):
    values, end = [], time.monotonic() + seconds
    while time.monotonic() < end:
        values.append(memory(pid)["ussBytes"])
        time.sleep(0.1)
    return int(statistics.median(values))


def main():
    parser = argparse.ArgumentParser(description="LayerE dialog/profile-switch cycles")
    parser.add_argument("--session", required=True)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    pid = json.loads((args.out / "editor-identity.json").read_text())["pid"]
    walk = Walk(args.session)
    report: dict[str, object] = {"warmup": WARMUP, "cycles": 0, "driftLimitBytes": DRIFT_LIMIT,
                                 "passed": False}
    try:
        walk.capture("cycles-start")
        walk.focus(EDITOR, "cycles-focused")
        nodes = walk.nodes()
        rows = walk.find("list", "Profiles", nodes)
        listed = sum(1 for n in nodes if n["role"] == "list item" and n["depth"] == rows["depth"] + 1)
        check(listed == PROFILES, f"representative store lists {listed} profiles, not {PROFILES}")
        delete = walk.find("button", "Delete", nodes)

        def cycle(index):
            walk.row(index % ROWS, f"cycle-{index:03d}-row")
            walk.click(delete, f"cycle-{index:03d}-dialog")
            walk.key("Escape", f"cycle-{index:03d}-closed")

        for index in range(WARMUP):
            cycle(index)
        check(walk.dialog() is None, "warm-up left a dialog open")
        baseline = settled_uss(pid, 2.0)
        checkpoints = []
        for index in range(CYCLES):
            cycle(WARMUP + index)
            report["cycles"] = index + 1
            if (index + 1) % CHECKPOINT == 0:
                check(walk.dialog() is None, f"cycle {index + 1} left its dialog open")
                checkpoints.append({"cycle": index + 1, "title": walk.window(EDITOR)["name"],
                                    "ussBytes": settled_uss(pid, 0.5)})
        titles = {c["title"] for c in checkpoints}
        check(len(titles) > 1, "the cycles never switched profiles")
        final = settled_uss(pid, 3.0)
        tail = [c["ussBytes"] for c in checkpoints[-4:]]
        rising = all(b > a for a, b in zip(tail, tail[1:], strict=False)) and \
            tail[-1] - tail[0] > TREND_FLOOR
        report.update(baselineUssBytes=baseline, finalUssBytes=final,
                      driftBytes=final - baseline, checkpoints=checkpoints,
                      continuingUpwardTrend=rising, **memory(pid))
        check(final - baseline <= DRIFT_LIMIT,
              f"USS drift {(final - baseline) / MIB:.3f} MiB exceeds 2 MiB")
        check(not rising, "USS keeps rising across the final checkpoints")
        report["passed"] = True
    except Exception as error:  # noqa: BLE001 -- recorded, the report decides
        report["error"] = f"{type(error).__name__}: {error}"
    report["screenshots"] = len(walk.shots)
    (args.out / "cycles.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    print(("PASS" if report["passed"] else "FAIL") + f": {report['cycles']} cycles; "
          + str(report.get("error") or f"drift {report.get('driftBytes')} bytes"), flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
