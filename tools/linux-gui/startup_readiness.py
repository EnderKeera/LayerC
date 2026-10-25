#!/usr/bin/env python3
"""Startup-only readiness transport; never substitutes for resource evidence."""
import argparse
import errno
import json
from pathlib import Path


def announced(log: Path, report: dict) -> bool:
    """Retry only a transient Linux ENODATA read, inside the existing deadline.

    Some WSL/DrvFS reads return ENODATA while the owned log is being written.
    Retain the retry count; do not announce readiness or change any sample on
    that failure. All other I/O failures remain failures.
    """
    try:
        lines = log.read_text(errors="replace").splitlines()
    except FileNotFoundError:
        return False
    except OSError as error:
        if error.errno != errno.ENODATA:
            raise
        report["readinessLogNoDataRetries"] = report.get("readinessLogNoDataRetries", 0) + 1
        return False
    return "ready" in lines


def verify_report(path: Path, pid: int, start: str, nonce: str) -> None:
    """Fail closed before the harness addresses the sampled application's window."""
    report = json.loads(path.read_text(encoding="utf-8"))
    if report.get("passed") is not True:
        raise RuntimeError("startup sampler failed: " + str(report.get("error", "no success receipt")))
    actual = (report.get("applicationPid"), report.get("applicationStartTime"), report.get("nonce"))
    if actual != (pid, start, nonce):
        raise ValueError("startup sampler identity mismatch")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("pid", type=int)
    parser.add_argument("start")
    parser.add_argument("nonce")
    args = parser.parse_args()
    verify_report(args.report, args.pid, args.start, args.nonce)


if __name__ == "__main__":
    main()
