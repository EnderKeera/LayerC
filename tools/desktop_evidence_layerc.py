#!/usr/bin/env python3
"""Convert LayerC benchmark and physical-X11 evidence into desktop reports."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

MODULE_PATH = Path(__file__).with_name("desktop_evidence.py")
SPEC = importlib.util.spec_from_file_location("desktop_evidence", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise ImportError(f"could not load desktop evidence validator from {MODULE_PATH}")
subject = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = subject
SPEC.loader.exec_module(subject)

HARNESS_VERSION = "1"
REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
WINDOWS_IDS = ("v11-visual-low", "v11-visual-balanced", "v21-template", "v22-latency")
LINUX_STAGES = ("verify-headless", "verify-x11", "native-headless-smoke", "native-x11-smoke")


def sanitize_text(value: str, maximum: int = 80) -> str:
    value = "".join(char if subject.text_character(char) else " " for char in value)
    return re.sub(r" +", " ", value).strip()[:maximum].rstrip(" ")


def read_object(path: Path) -> dict:
    return subject.parse_report(path.read_text(encoding="utf-8"))


def base_report(args: argparse.Namespace, harness: str, session: str,
                recorded_at: str, build: dict) -> dict:
    environment = {"os": sanitize_text(args.os) or "unknown", "session": session,
                   "machine": args.machine}
    gpu = sanitize_text(getattr(args, "gpu", ""))
    if gpu:
        environment["gpu"] = gpu
    return {"schema": subject.SCHEMA, "project": args.repository, "commit": args.commit,
            "recordedAt": recorded_at, "harness": {"id": harness, "version": HARNESS_VERSION},
            "build": build, "environment": environment, "results": []}


def windows_report(args: argparse.Namespace) -> dict:
    identity_bytes = args.identity.read_bytes()
    identity = subject.parse_report(identity_bytes.decode("utf-8"))
    if (identity.get("source") != "ci-artifact" or
            not subject.matches(subject.COMMIT, identity.get("commitSha")) or
            type(identity.get("runId")) is not str or
            re.fullmatch(r"[0-9]+", identity["runId"]) is None):
        raise ValueError("invalid CI artifact build identity")
    run_id, built_commit = identity["runId"], identity["commitSha"]
    try:
        ci_run_id = int(run_id)
    except ValueError as error:
        raise ValueError("invalid CI artifact runId") from error
    build = {"source": "ci-artifact", "ciRunId": ci_run_id, "builtCommit": built_commit,
             "identitySha256": hashlib.sha256(identity_bytes).hexdigest()}
    recorded_at = args.recorded_at or datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    report = base_report(args, "layerc-windows-hardware", "windows-desktop", recorded_at, build)
    for test_id in WINDOWS_IDS:
        path = args.results / (test_id + ".json")
        status = "fail"
        if not path.exists():
            if args.visual_only and test_id == "v22-latency":
                status, detail = "skip", "not run (VisualOnly)"
            else:
                detail = "no result: the benchmark did not complete"
        else:
            try:
                result = read_object(path)
            except (OSError, UnicodeError, ValueError):
                detail = "unreadable result"
            else:
                identity_match = result.get("build")
                if (type(identity_match) is not dict or
                        identity_match.get("source") != "ci-artifact" or
                        str(identity_match.get("runId")) != run_id or
                        identity_match.get("commitSha") != built_commit):
                    detail = "result build identity does not match the CI artifact"
                elif result.get("passed") is True:
                    status, detail = "pass", "passed"
                elif result.get("passed") is False:
                    detail = "thresholds not met"
                else:
                    detail = "result has no boolean passed"
        report["results"].append({"id": test_id, "status": status, "detail": detail})
    return report


def linux_report(args: argparse.Namespace) -> dict:
    summary = read_object(args.evidence / "summary.json")
    if summary.get("desktop") != args.desktop:
        raise ValueError("evidence desktop does not match --desktop")
    if summary.get("packageMode") != args.package_mode:
        raise ValueError("evidence package mode does not match --package-mode")
    head = (args.evidence / "git-head.txt").read_text(encoding="utf-8").strip()
    if head != args.commit:
        raise ValueError("evidence git-head.txt does not match --commit")
    automated_path = args.evidence / "automated-summary.json"
    if not automated_path.is_file():
        raise ValueError("evidence has no automated summary; rerun with "
                         "LAYERC_RUN_AUTOMATION=1 LAYERC_REQUIRE_AUTOMATION=1")
    automated_summary = read_object(automated_path)
    if automated_summary.get("commit") != args.commit:
        raise ValueError("automated summary commit does not match --commit")
    if automated_summary.get("sourceTreeClean") is not True:
        raise ValueError("automated summary sourceTreeClean must be JSON true")
    if args.package_mode == "flatpak":
        message = "the harness did not run the Flatpak this helper installed"
        if (type(args.flatpak_commit) is not str or
                re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", args.flatpak_commit) is None):
            raise ValueError(message)
        try:
            info = (args.evidence / "flatpak-info.txt").read_text(encoding="utf-8")
        except OSError as error:
            raise ValueError(message) from error
        if not any(line.strip() == "Commit: " + args.flatpak_commit for line in info.splitlines()):
            raise ValueError(message)
    recorded_at = datetime.strptime(summary["recordedAtUtc"], "%Y%m%dT%H%M%SZ").strftime("%Y-%m-%dT%H:%M:%SZ")
    report = base_report(args, "layerc-linux-x11-" + args.package_mode,
                         "gnome-xorg" if args.desktop == "gnome" else "kde-x11", recorded_at,
                         {"source": "local-build", "builtCommit": automated_summary["commit"], "dirty": False})
    manual = summary.get("manualResult")
    if manual not in ("pass", "fail", "pending"):
        raise ValueError("evidence manualResult must be pass, fail, or pending")
    report["results"].append({"id": "manual-checklist",
                              "status": "unavailable" if manual == "pending" else manual,
                              "detail": "operator result pending" if manual == "pending" else
                              "operator recorded " + manual})
    automated = automated_summary.get("automatedEvidence", {})
    stages = LINUX_STAGES + (("flatpak",) if args.package_mode == "flatpak" else ())
    for stage in stages:
        entry = automated.get(stage, {}) if type(automated) is dict else {}
        value = entry.get("status", "missing") if type(entry) is dict else "missing"
        status = "skip" if value == "skipped" else value if value in ("pass", "fail", "skip") else "unavailable"
        detail = sanitize_text("automated stage status " + str(value), 200)
        report["results"].append({"id": stage, "status": status, "detail": detail})
    return report


def main(argv: list[str] | None = None) -> int:
    parser = subject._ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    windows = commands.add_parser("windows-report")
    windows.add_argument("--results", type=Path, required=True)
    windows.add_argument("--identity", type=Path, required=True)
    windows.add_argument("--gpu", default="")
    windows.add_argument("--visual-only", action="store_true")
    windows.add_argument("--recorded-at")
    windows.add_argument("--built-commit-parents", type=Path)
    linux = commands.add_parser("linux-report")
    linux.add_argument("--evidence", type=Path, required=True)
    linux.add_argument("--desktop", choices=("gnome", "kde"), required=True)
    linux.add_argument("--package-mode", choices=("native", "flatpak"), required=True)
    linux.add_argument("--flatpak-commit")
    for sub in (windows, linux):
        sub.add_argument("--commit", required=True)
        sub.add_argument("--repository", required=True)
        sub.add_argument("--os", required=True)
        sub.add_argument("--machine", choices=("physical", "vm"), required=True)
        sub.add_argument("--out", required=True)
        sub.add_argument("--config", type=Path, default=REPOSITORY_ROOT / ".github/desktop-evidence.json")
    try:
        args = parser.parse_args(argv)
        config = subject.load_config(args.config)
        parents_path = getattr(args, "built_commit_parents", None)
        built_parents = subject.load_built_commit_parents(parents_path) if parents_path else None
        report = windows_report(args) if args.command == "windows-report" else linux_report(args)
        problems = subject.validate_report(report, config, args.repository, built_parents=built_parents)
        if problems:
            for problem in problems:
                print("rejected: " + problem, file=sys.stderr)
            return 2
        subject.write_output(args.out, json.dumps(report, indent=2, ensure_ascii=False) + "\n")
        return 0
    except (OSError, UnicodeError, ValueError, KeyError, TypeError, RecursionError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
