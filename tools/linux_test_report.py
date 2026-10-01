#!/usr/bin/env python3
"""Validate a CTest run against its gate's checked-in suite policy.

The expected suites come from tools/test-gates.json, never from what the build
happened to register: a suite that stops being registered, or loses its gate
label, fails the gate instead of silently leaving it.
"""

from __future__ import annotations

import argparse
import json
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

DEFAULT_POLICY = Path(__file__).with_name("test-gates.json")

@dataclass(frozen=True)
class Gate:
    """One gate's suites. Optional suites run only where registered."""

    name: str
    required: frozenset[str]
    label: str | None = None
    optional: frozenset[str] = field(default_factory=frozenset)
    allow_skip: frozenset[str] = field(default_factory=frozenset)

def load_gate(policy: Path, name: str) -> Gate:
    document = json.loads(policy.read_text(encoding="utf-8"))
    gates = document["gates"]
    if name not in gates:
        raise ValueError(f"unknown gate {name!r}; known: {', '.join(sorted(gates))}")
    entry = gates[name]
    gate = Gate(
        name=name,
        required=frozenset(entry["required"]),
        label=entry.get("label"),
        optional=frozenset(entry.get("optional", {})),
        allow_skip=frozenset(entry.get("allowSkip", {})),
    )
    if not gate.required:
        raise ValueError(f"gate {name!r} requires no suites")
    if gate.required & gate.optional:
        raise ValueError(f"gate {name!r} lists suites as both required and optional")
    if not gate.allow_skip <= gate.required | gate.optional:
        raise ValueError(f"gate {name!r} allows skips for suites outside the gate")
    return gate


def registered_names(path: Path, labels: tuple[str, ...] = ()) -> set[str]:
    document = json.loads(path.read_text(encoding="utf-8"))
    tests = document.get("tests", [])
    if labels:
        tests = [
            test for test in tests
            if any(
                prop.get("name") == "LABELS"
                and any(label in prop.get("value", []) for label in labels)
                for prop in test.get("properties", [])
            )
        ]
    return {test["name"] for test in tests}


def junit_details(path: Path) -> tuple[dict[str, int], dict[str, ET.Element]]:
    root = ET.parse(path).getroot()
    cases = root.findall(".//testcase")
    counts = {
        "tests": len(cases),
        "skipped": sum(case.find("skipped") is not None for case in cases),
        "failures": sum(case.find("failure") is not None for case in cases),
        "errors": sum(case.find("error") is not None for case in cases),
    }
    return counts, {case.get("name", ""): case for case in cases}


def skip_reason(case: ET.Element) -> str:
    """The lines a skipped suite printed about why, from its captured output."""
    output = case.findtext("system-out") or ""
    return " | ".join(
        line.strip() for line in output.splitlines()
        if line.lstrip().lower().startswith("skip")
    )


def validate(registration: Path, junit: Path, gate: Gate) -> dict[str, object]:
    registered = registered_names(registration)
    labelled = (
        registered_names(registration, (gate.label,)) if gate.label else registered
    )
    expected = gate.required | (gate.optional & registered)
    counts, cases = junit_details(junit)
    missing = sorted(gate.required - registered)
    unlabelled = sorted((expected & registered) - labelled) if gate.label else []
    unlisted = sorted(labelled - gate.required - gate.optional)
    missing_executed = sorted(expected - cases.keys())
    unexpected = sorted(cases.keys() - expected)
    unregistered = sorted(cases.keys() - registered)
    unexpected_skips = sorted(
        name for name, case in cases.items()
        if case.find("skipped") is not None and name not in gate.allow_skip
    )
    not_run = sorted(
        name for name, case in cases.items()
        if case.get("status") in ("notrun", "disabled")
    )
    problems: list[str] = []
    if missing:
        problems.append("required tests are not registered: " + ", ".join(missing))
    if unlabelled:
        problems.append(
            f"gate suites lack the {gate.label} label: " + ", ".join(unlabelled)
        )
    if unlisted:
        problems.append(
            f"registered {gate.label or 'suites'} not in the {gate.name} gate policy: "
            + ", ".join(unlisted)
        )
    if missing_executed:
        problems.append("required tests did not run: " + ", ".join(missing_executed))
    if counts["tests"] == 0:
        problems.append("CTest reported no test cases")
    if unexpected:
        problems.append("unexpected suites in CTest report: " + ", ".join(unexpected))
    if unregistered:
        problems.append("unregistered suites in CTest report: " + ", ".join(unregistered))
    if len(cases) != counts["tests"]:
        problems.append("duplicate testcase names in CTest report")
    if unexpected_skips:
        problems.append("unexpected skipped suites: " + ", ".join(unexpected_skips))
        problems.extend(
            f"{name} skip reason: {reason}"
            for name in unexpected_skips
            if (reason := skip_reason(cases[name]))
        )
    if not_run:
        problems.append("tests did not run: " + ", ".join(not_run))
    if counts["failures"] or counts["errors"]:
        problems.append(
            f"CTest XML contains {counts['failures']} failure(s) and "
            f"{counts['errors']} error(s)"
        )
    return {
        **counts,
        "gate": gate.name,
        "required": sorted(gate.required),
        "optionalRun": sorted(gate.optional & cases.keys()),
        "allowedSkips": sorted(
            name for name in gate.allow_skip
            if name in cases and cases[name].find("skipped") is not None
        ),
        "missingRequired": missing,
        "missingExecuted": missing_executed,
        "unexpected": unexpected,
        "result": "pass" if not problems else "fail",
        "problems": problems,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--registration", required=True, type=Path)
    parser.add_argument("--junit", required=True, type=Path)
    parser.add_argument("--gate", required=True)
    parser.add_argument("--policy", type=Path, default=DEFAULT_POLICY)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()

    try:
        report = validate(
            args.registration, args.junit, load_gate(args.policy, args.gate)
        )
    except (OSError, ValueError, KeyError, ET.ParseError) as error:
        report = {
            "result": "fail",
            "problems": [f"could not parse CTest evidence: {error}"],
        }
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if report["result"] != "pass":
        problems = report["problems"]
        assert isinstance(problems, list)
        print("error: " + "; ".join(str(problem) for problem in problems), file=sys.stderr)
        return 1
    print(f"Verified {report['tests']} CTest suites against the {args.gate} gate")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
