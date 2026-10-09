#!/usr/bin/env python3
"""Compose result.json for one Linux GUI run from its evidence files.

  report.py OUT MODE SESSION_EXIT JOINED SANDBOX_REMOVED LEFTOVERS LOCK_LEFT
            STALE_LOCKS [ERROR]

A run passes only if every required case passed (walkthrough mode), the
editor resource targets hold, every expected owned process stopped as
intended, and cleanup left nothing behind. Manifests are authoritative:
missing, duplicate, unexpected or malformed evidence is a failure, never a
skip, and missing measurements never default to zero.
"""
import json
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from walkthrough import EXPECTED  # noqa: E402 -- the required-case manifest

MIB = 1024 * 1024
TARGETS = {"idleUssBytes": 20 * MIB, "startupPeakUssBytes": 20 * MIB, "idleCpuPercent": 0.2}
# Approved ceilings for the representative store (docs/LinuxEditorPlan.md §7).
REPRESENTATIVE = {"idleUssBytes": 32 * MIB, "startupPeakUssBytes": 32 * MIB, "idleCpuPercent": 0.2}
CYCLE_COUNT, CYCLE_DRIFT = 100, 2 * MIB
OWNED_ROLES = ("wm", "fixture", "daemon", "sampler", "bridge", "outer-xvfb")
MIN_IDLE_SECONDS = 5.0


def load(path):
    try:
        return json.loads(path.read_text(), parse_constant=lambda name: None)
    except (OSError, ValueError):
        return None


def is_true(value):
    """Strict JSON true: 1, "true" or other truthy values do not count."""
    return isinstance(value, bool) and value


def number(source, key, failures, label):
    value = source.get(key) if isinstance(source, dict) else None
    try:
        finite = isinstance(value, (int, float)) and not isinstance(value, bool) \
            and math.isfinite(value) and value >= 0 and value < 2**63
    except OverflowError:
        finite = False
    if not finite:
        failures.append(f"{label}: {key} is missing or not a finite nonnegative number")
        return None
    return value


def host_class(out):
    environment = load(out / "environment.json") or {}
    release = str((environment.get("kernel") or {}).get("release", "")).lower()
    if "microsoft" in release or "wsl" in release:
        return "WSL (observed kernel)"
    return "Linux host; native, VM or container not verified"


def check_cases(out, failures):
    report = load(out / "cases.json")
    if not isinstance(report, dict) or not isinstance(report.get("cases"), list):
        failures.append("walkthrough produced no case report")
        return None, None
    seen = {}
    for case in report["cases"]:
        name = case.get("id") if isinstance(case, dict) else None
        if not isinstance(name, str):
            failures.append(f"malformed case record: {case!r:.120}")
            continue
        if name in seen:
            failures.append(f"duplicate case record: {name}")
        seen[name] = case
    unexpected = [name for name in seen if name not in EXPECTED]
    missing = [name for name in EXPECTED if name not in seen]
    if unexpected:
        failures.append(f"unexpected case records: {unexpected}")
    if missing:
        failures.append(f"required cases missing: {', '.join(missing)}")
    bad = [name for name in EXPECTED if name in seen and (
        not is_true(seen[name].get("required")) or seen[name].get("status") != "pass")]
    if bad:
        failures.append(f"required cases not passed: {', '.join(bad)}")
    statuses = {name: case.get("status") for name, case in seen.items() if isinstance(case, dict)}
    return statuses, report.get("deferred")


def check_cycles(out, failures):
    report = load(out / "cycles.json")
    if not isinstance(report, dict):
        failures.append("cycle run produced no report")
        return None
    drift = number(report, "driftBytes", failures, "cycles") if "driftBytes" in report else None
    if report.get("cycles") != CYCLE_COUNT:
        failures.append(f"only {report.get('cycles')} of {CYCLE_COUNT} cycles completed")
    if drift is None or drift > CYCLE_DRIFT or report.get("continuingUpwardTrend") is not False:
        failures.append(f"cycle USS target not met: {report.get('error') or drift}")
    if not is_true(report.get("passed")):
        failures.append(f"cycle run failed: {report.get('error')}")
    return {k: report.get(k) for k in ("cycles", "baselineUssBytes", "finalUssBytes",
                                       "driftBytes", "continuingUpwardTrend")}

def main():
    out = Path(sys.argv[1])
    try:
        return evaluate(out)
    except Exception as error:  # noqa: BLE001 -- malformed evidence must still fail visibly
        result = {"mode": sys.argv[2], "passed": False,
                  "failures": [f"report could not evaluate the evidence: {type(error).__name__}: {error}"]}
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(f"FAIL: {out}\n  - {result['failures'][0]}")
        return 1


def evaluate(out):
    mode = sys.argv[2]
    session_exit, joined, sandbox_removed = int(sys.argv[3]), sys.argv[4] == "1", sys.argv[5] == "1"
    leftovers, lock_left, stale_locks = sys.argv[6], sys.argv[7] == "1", sys.argv[8]
    setup_error = sys.argv[9] if len(sys.argv) > 9 else ""
    failures = [setup_error] if setup_error else []

    cleanup = {"ownedProcessesJoined": joined, "leftoverProcesses": leftovers.split(),
               "displayLocksRemoved": not lock_left, "sandboxRemoved": sandbox_removed,
               "staleOwnLocksRemoved": stale_locks.split()}
    (out / "cleanup.json").write_text(json.dumps(cleanup, indent=2) + "\n")
    if not joined or not sandbox_removed or leftovers.strip() or lock_left:
        failures.append("cleanup left owned resources behind")

    ownership = {p.stem: load(p) for p in sorted((out / "ownership").glob("*.status"))}
    if not setup_error:
        for role in OWNED_ROLES:
            if role not in ownership:
                failures.append(f"owned process record missing: {role}")
    for name, status in ownership.items():
        if name == "outer-session" or name.startswith("xvfb-attempt-"):
            continue  # Session exit is reported below; retried attempts only need their locks gone.
        code = status.get("code") if isinstance(status, dict) else None
        if not isinstance(status, dict) or status.get("state") not in ("stopped", "exited") \
                or isinstance(code, bool) or not isinstance(code, int) or code != 0:
            failures.append(f"owned process {name} ended abnormally: {status}")

    targets = REPRESENTATIVE if mode == "resources" else TARGETS
    resources: dict[str, object] = {"targets": targets}
    idle, startup = load(out / "resources.json"), load(out / "startup-resources.json")
    if not isinstance(idle, dict) or not isinstance(startup, dict):
        failures.append("editor resource evidence is missing")
    else:
        startup_kib = number(startup, "peakUssKiB", failures, "startup")
        observed = {"idleUssBytes": number(idle, "peakIdleUssBytes", failures, "idle"),
                    "startupPeakUssBytes": None if startup_kib is None else startup_kib * 1024,
                    "idleCpuPercent": number(idle, "idleCpuPercent", failures, "idle"),
                    "idleSeconds": number(idle, "idleSeconds", failures, "idle")}
        resources["observed"] = observed
        for key, limit in targets.items():
            if observed[key] is not None and observed[key] > limit:
                failures.append(f"resource target exceeded: {key}")
        if observed["idleSeconds"] is not None and observed["idleSeconds"] < MIN_IDLE_SECONDS:
            failures.append(f"idle window shorter than {MIN_IDLE_SECONDS} s")
        if not (is_true(startup.get("passed")) and is_true(startup.get("stoppedByHarness"))):
            failures.append("startup sampler did not complete and stop LayerE cleanly")

    result: dict[str, object] = {
        "mode": mode, "evidenceClass": "automated private-Xvfb session",
        "host": host_class(out), "sessionExit": session_exit,
        "resources": resources, "ownership": ownership}
    if mode == "walkthrough":
        statuses, deferred = check_cases(out, failures)
        if statuses is not None:
            result["cases"] = statuses
            result["deferred"] = deferred
    elif mode == "resources":
        result["fixture"] = "representative: 20 profiles x 10 cooldowns x 2 panels"
        result["cycles"] = check_cycles(out, failures)
    else:
        result["note"] = ("Agent session: actions.jsonl and shots/ record the agent's "
                          "coverage; this run makes no scripted case claims.")
    if session_exit != 0 and not any("required cases" in f or "cycle" in f for f in failures):
        failures.append(f"session exited with status {session_exit}")
    result.update(passed=not failures, failures=failures)
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(("PASS" if not failures else "FAIL") + f": {out}")
    for failure in failures:
        print(f"  - {failure}")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
