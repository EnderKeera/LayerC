#!/usr/bin/env python3
"""Pre-exec-gated startup sampler and sole owner of the measured application.

Linux/Xvfb test harness, never production. The sampler runs under owned.py;
SIGTERM from that owner requests an orderly stop. Nothing else signals the
application, which stays unreaped here until it is stopped, so its PID cannot
be reused while it is sampled or signalled. The PID file ("pid starttime") is
published only after the application image has been exec'd. The report is
bound to this run by a nonce plus the application's PID/start time/executable.
"""
import argparse
import contextlib
import json
import os
import select
import signal
import time
from pathlib import Path

os.closerange(3, 65536)  # Inherited harness descriptors are not ours to hold.
parser = argparse.ArgumentParser()
parser.add_argument("executable", type=Path)
parser.add_argument("log", type=Path)
parser.add_argument("pid_file", type=Path)
parser.add_argument("report", type=Path)
parser.add_argument("settle_seconds", type=float)
parser.add_argument("nonce")
args = parser.parse_args()
exe = args.executable.resolve(strict=True)
report = {"passed": False, "nonce": args.nonce, "executable": str(exe),
          "sampleIntervalSeconds": .005, "sampleCount": 0,
          "peakUssKiB": 0, "peakPssKiB": 0, "peakRssKiB": 0,
          "samplingEstablishedBeforeExec": True}


class StopRequest(Exception):
    pass


def request_stop(signum, frame):
    raise StopRequest(signum)


def save_report():
    temporary = args.report.with_name(args.report.name + ".tmp")
    temporary.write_text(json.dumps(report, indent=2) + "\n")
    temporary.replace(args.report)


def start_time(proc: Path) -> str:
    # Field 22 follows the parenthesized, possibly space-containing comm.
    return (proc / "stat").read_text().rsplit(")", 1)[1].split()[19]


def memory(proc: Path):
    values = {}
    for line in (proc / "smaps_rollup").read_text().splitlines():
        parts = line.split()
        if parts and parts[0] in ("Private_Clean:", "Private_Dirty:", "Pss:", "Rss:"):
            values[parts[0]] = int(parts[1])
    if len(values) != 4:
        raise RuntimeError("incomplete smaps_rollup sample")
    return values


def stop_application(pidfd):
    """Bounded stop of the unreaped child; returns its wait status."""
    for signum, seconds in ((signal.SIGTERM, 3), (signal.SIGKILL, 1)):
        signal.pidfd_send_signal(pidfd, signum)
        if select.select([pidfd], [], [], seconds)[0]:
            return signum
    raise TimeoutError("application could not be stopped")


pid = None
pidfd = -1
gate_read, gate_write = os.pipe()
signal.signal(signal.SIGTERM, request_stop)
signal.signal(signal.SIGINT, request_stop)
try:
    pid = os.fork()  # Before any threads exist; child waits for sampler setup.
    if pid == 0:
        os.close(gate_write)
        try:
            signal.signal(signal.SIGTERM, signal.SIG_DFL)
            signal.signal(signal.SIGINT, signal.SIG_DFL)
            if os.read(gate_read, 1) != b"x":
                os._exit(126)
            os.close(gate_read)
            with args.log.open("wb", buffering=0) as log:
                os.dup2(log.fileno(), 1)
                os.dup2(log.fileno(), 2)
                os.execv(str(exe), [str(exe)])  # noqa: S606 -- isolated executable, no shell
        except BaseException:
            os._exit(127)
    os.close(gate_read)
    gate_read = -1
    pidfd = os.pidfd_open(pid)
    proc = Path("/proc") / str(pid)
    started = time.monotonic()
    ready_at = None
    first_sample = None
    os.write(gate_write, b"x")
    os.close(gate_write)
    gate_write = -1
    deadline = started + 30
    try:
        while True:
            now = time.monotonic()
            if select.select([pidfd], [], [], 0)[0]:
                raise RuntimeError("application exited during startup")
            # Never count the forked Python image as application memory.
            if (proc / "exe").resolve() == exe:
                values = memory(proc)
                uss = values["Private_Clean:"] + values["Private_Dirty:"]
                report["peakUssKiB"] = max(report["peakUssKiB"], uss)
                report["peakPssKiB"] = max(report["peakPssKiB"], values["Pss:"])
                report["peakRssKiB"] = max(report["peakRssKiB"], values["Rss:"])
                report["sampleCount"] += 1
                if first_sample is None:
                    first_sample = now
                    # Publish only once the image is the prototype itself.
                    report.update(applicationPid=pid, applicationStartTime=start_time(proc))
                    temporary = args.pid_file.with_name(args.pid_file.name + ".tmp")
                    temporary.write_text(f"{pid} {report['applicationStartTime']}\n")
                    temporary.replace(args.pid_file)
            if (ready_at is None and args.log.exists()
                    and "ready" in args.log.read_text(errors="replace").splitlines()):
                ready_at = now
                deadline = now + args.settle_seconds
                report["readySecondsAfterRelease"] = round(now - started, 6)
            if now >= deadline:
                if ready_at is None:
                    raise TimeoutError("application did not announce readiness")
                break
            time.sleep(.005)
    except StopRequest:
        raise RuntimeError("stop requested before startup sampling completed") from None
    if not report["sampleCount"] or first_sample is None:
        raise RuntimeError("no post-exec memory samples")
    report.update(passed=True,
                  firstSampleSecondsAfterRelease=round(first_sample - started, 6),
                  sampledSeconds=round(time.monotonic() - started, 6),
                  note="Sampled peak, not a proof of unsampled sub-interval allocation peaks")
    save_report()
    # Supervise without polling: the pidfd becomes readable only on exit.
    try:
        select.select([pidfd], [], [])
        report["passed"] = False
        raise RuntimeError("application exited unexpectedly")
    except StopRequest:
        pass
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    used = stop_application(pidfd)
    _, status = os.waitpid(pid, 0)
    pid = None
    exit_code = os.waitstatus_to_exitcode(status)
    # Only the intended outcomes count as an orderly stop: a clean exit or
    # termination by the SIGTERM sent here. A crash or other exit fails.
    orderly = used == signal.SIGTERM and exit_code in (0, -signal.SIGTERM)
    report.update(stoppedByHarness=orderly, applicationExitCode=exit_code)
    if not orderly:
        report["passed"] = False
        report["error"] = f"application did not stop cleanly (exit status {exit_code})"
except BaseException as error:
    report["passed"] = False
    report.setdefault("error", f"{type(error).__name__}: {error}")
    save_report()
    raise
finally:
    for fd in (gate_read, gate_write):
        if fd >= 0:
            os.close(fd)
    if pid is not None:
        # Still unreaped, so the PID is ours: stop and reap it boundedly.
        if pidfd >= 0:
            with contextlib.suppress(OSError, TimeoutError):
                stop_application(pidfd)
        os.waitpid(pid, 0)
    if pidfd >= 0:
        os.close(pidfd)
    save_report()
