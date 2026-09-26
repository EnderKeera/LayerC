#!/usr/bin/env python3
"""Validate that a CTest run exercised every registered suite in its gate."""

from __future__ import annotations

import argparse
import json
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


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


def validate(
    registration: Path,
    junit: Path,
    required: list[str],
    *,
    labels: tuple[str, ...] = (),
    allow_skip: tuple[str, ...] = (),
) -> dict[str, object]:
    registered = registered_names(registration)
    selected = registered_names(registration, labels) if labels else registered
    required_names = set(required) | selected if labels else set(required)
    counts, cases = junit_details(junit)
    missing = sorted(required_names - registered)
    missing_executed = sorted(required_names - cases.keys())
    unexpected = sorted(cases.keys() - required_names)
    unregistered = sorted(cases.keys() - registered)
    unexpected_skips = sorted(
        name for name, case in cases.items()
        if case.find("skipped") is not None and name not in allow_skip
    )
    not_run = sorted(
        name for name, case in cases.items()
        if case.get("status") in ("notrun", "disabled")
    )
    problems: list[str] = []
    if not required_names:
        problems.append("no required tests were selected")
    if missing:
        problems.append("required tests are not registered: " + ", ".join(missing))
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
        "required": sorted(required_names),
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
    parser.add_argument("--required", action="append", default=[])
    parser.add_argument("--label", action="append", default=[])
    parser.add_argument("--allow-skip", action="append", default=[])
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()

    try:
        report = validate(
            args.registration, args.junit, args.required,
            labels=tuple(args.label), allow_skip=tuple(args.allow_skip),
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
    print(f"Verified {report['tests']} CTest suites against registration")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
