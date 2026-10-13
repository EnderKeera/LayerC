"""Read-only idle probe, confined to the runner-owned LayerE process."""
import json
import os
import select
import time
from pathlib import Path

from sample import cpu_seconds, memory

ADVANCED_VIEWS = ("json", "simulator", "trace-stopped", "calibration-stopped",
                  "autosave-stopped", "live-draft-active", "diagnostics-open")
SAMPLE_SECONDS = 60.0
SAMPLE_INTERVAL = 0.25
# The editor must share the bridge's private roots, not only its display.
PRIVATE_ROOTS = ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_RUNTIME_DIR", "XAUTHORITY")


def read_identity(evidence):
    """The owned launch recorded by the runner: PID, start time, nonce, image."""
    try:
        identity = json.loads((evidence / "editor-identity.json").read_text())
        sampler = json.loads((evidence / "startup-resources.json").read_text())
    except (OSError, ValueError) as error:
        raise RuntimeError("missing or invalid owned-editor identity") from error
    if not isinstance(identity, dict) or not isinstance(sampler, dict):
        raise RuntimeError("owned-editor identity is not an object")
    pid, start, nonce = identity.get("pid"), identity.get("startTime"), identity.get("nonce")
    executable = sampler.get("executable")
    if (isinstance(pid, bool) or not isinstance(pid, int) or pid <= 0
            or not isinstance(start, str) or not start.isascii() or not start.isdigit()
            or not isinstance(nonce, str) or not nonce or nonce != sampler.get("nonce")
            or pid != sampler.get("applicationPid")
            or start != sampler.get("applicationStartTime") or sampler.get("passed") is not True
            or not isinstance(executable, str) or Path(executable).name != "layere"):
        raise RuntimeError("editor identity does not match its owned sampler")
    return pid, start, nonce, executable


def start_time(pid):
    return (Path(f"/proc/{pid}") / "stat").read_text().rsplit(")", 1)[1].split()[19]


def verify_editor(pid, start, executable, display, profiles):
    """Called with the pidfd already open, so the PID cannot be recycled."""
    proc = Path(f"/proc/{pid}")
    try:
        geteuid = getattr(os, "geteuid", None)
        if not callable(geteuid) or proc.stat().st_uid != geteuid():
            raise RuntimeError("editor PID is not owned by this user")
        if start_time(pid) != start or \
                (proc / "exe").resolve(strict=True) != Path(executable).resolve(strict=True):
            raise RuntimeError("editor process identity changed")
        environment = dict(item.split(b"=", 1) for item in (proc / "environ").read_bytes().split(b"\0")
                           if b"=" in item)
        expected = {name: os.environ[name] for name in PRIVATE_ROOTS}
        if (environment.get(b"DISPLAY") != os.fsencode(display)
                or expected["XDG_CONFIG_HOME"] != str(profiles.parent.parent)
                or any(environment.get(os.fsencode(name)) != os.fsencode(value)
                       for name, value in expected.items())):
            raise RuntimeError("editor does not belong to this private GUI session")
    except (OSError, KeyError, IndexError, ValueError) as error:
        raise RuntimeError("cannot confirm the owned editor process") from error


def exited(pidfd):
    return bool(select.select([pidfd], [], [], 0)[0])


def measure_editor(evidence, display, profiles, deadline):
    """Sixty monotonic idle seconds; never signal, restart or reap the editor.

    The pidfd is opened before the identity check and pins the process; a
    final start-time check and exit poll follow the last reading. Results are
    observed idle peaks, not a bound on unsampled allocation spikes.
    """
    pid, start, nonce, executable = read_identity(evidence)
    if deadline - time.monotonic() < SAMPLE_SECONDS + 5:
        raise RuntimeError("not enough operation budget for a full idle window")
    pidfd_open = getattr(os, "pidfd_open", None)
    if not callable(pidfd_open):
        raise RuntimeError("resource sampling requires Linux pidfd support")
    try:
        pidfd = pidfd_open(pid)
    except OSError as error:
        raise RuntimeError("owned editor is not running") from error
    if isinstance(pidfd, bool) or not isinstance(pidfd, int) or pidfd < 0:
        raise RuntimeError("invalid Linux pidfd")
    try:
        verify_editor(pid, start, executable, display, profiles)
        started, initial, samples = time.monotonic(), cpu_seconds(pid), []
        while time.monotonic() - started < SAMPLE_SECONDS:
            if time.monotonic() >= deadline:
                raise RuntimeError("resource sampling exceeded its operation budget")
            if exited(pidfd):
                raise RuntimeError("owned editor exited during resource sampling")
            samples.append({"monotonic": round(time.monotonic(), 6), **memory(pid)})
            time.sleep(SAMPLE_INTERVAL)
        elapsed = time.monotonic() - started
        cpu = cpu_seconds(pid) - initial
        if exited(pidfd) or start_time(pid) != start:
            raise RuntimeError("owned editor exited during resource sampling")
        return {"process": "layere", "pid": pid, "startTime": start, "nonce": nonce,
                "idleSeconds": round(elapsed, 3),
                "idleCpuPercent": round(100 * cpu / elapsed, 4),
                "peakIdleUssBytes": max(s["ussBytes"] for s in samples),
                "peakIdlePssBytes": max(s["pssBytes"] for s in samples),
                "peakIdleRssBytes": max(s["rssBytes"] for s in samples), "samples": samples}
    except OSError as error:
        raise RuntimeError("cannot sample the owned editor process") from error
    finally:
        os.close(pidfd)
