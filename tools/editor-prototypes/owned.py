#!/usr/bin/env python3
"""Owning supervisor for one prototype-harness process (or process tree).

Usage: owned.py [--group] - STATE_PREFIX -- COMMAND [ARG]...   < CONTROL_FIFO

Linux/Xvfb test harness only. Scripts never signal a numeric PID. This
supervisor keeps its child unreaped, so the PID (and, with --group, the
process-group ID) stays reserved for as long as it is signalled. Standard
input is the control FIFO's read end, opened by the launching shell while it
still holds the write end (so the open never blocks and no early close is
missed). Writing to the FIFO or closing the shell's end, including when the
shell dies, requests a bounded stop. Stop requests (also SIGTERM/SIGINT/SIGHUP) are
deferred while the child is spawned and published, so a created child is never
lost; every exit path stops and reaps it.

With --group the supervisor is a child subreaper: the command runs in a new
session, and descendants that leave its process group (setsid/setpgid) are
re-parented here when orphaned and stopped through pidfds as well.

STATE_PREFIX.pid receives the child PID once it is spawned. STATE_PREFIX.status
is written last: {"state": "stopped"|"exited"|"failed", "code": N}.
"""
import contextlib
import json
import os
import select
import signal
import subprocess
import sys
import time
from pathlib import Path

PR_SET_CHILD_SUBREAPER = 36
STOP_SIGNALS = {signal.SIGTERM, signal.SIGINT, signal.SIGHUP}


class StopRequest(Exception):
    pass


def request_stop(signum, frame):
    raise StopRequest(signum)


def stat_fields(pid):
    try:
        return (Path("/proc") / str(pid) / "stat").read_text().rsplit(")", 1)[1].split()
    except (OSError, IndexError):
        return None


def processes():
    for entry in Path("/proc").iterdir():
        if entry.name.isdecimal():
            fields = stat_fields(int(entry.name))
            if fields:
                yield int(entry.name), fields


def group_members(pgid):
    """Live (non-zombie) processes in the group."""
    return [pid for pid, fields in processes() if int(fields[2]) == pgid and fields[0] != "Z"]


def adopted_children(leader):
    """Our children other than the leader: re-parented (orphaned) descendants."""
    me = str(os.getpid())
    return [pid for pid, fields in processes() if fields[1] == me and pid != leader]


def stop_adopted(leader, seconds):
    """Stop and reap re-parented descendants through pidfds; they are our
    unreaped children, so their PIDs cannot be reused while we hold them.
    Returns the descendants still present after the bound (normally none)."""
    deadline = time.monotonic() + seconds
    escalated = False
    while time.monotonic() < deadline:
        children = adopted_children(leader)
        if not children:
            return []
        for pid in children:
            fields = stat_fields(pid)
            if fields and fields[0] != "Z":
                sig = signal.SIGKILL if escalated else signal.SIGTERM
                try:
                    fd = os.pidfd_open(pid)
                except OSError:
                    # pidfds unavailable: the PID of our own unreaped child
                    # stays reserved until we reap it, so it is safe to signal.
                    with contextlib.suppress(ProcessLookupError):
                        os.kill(pid, sig)
                else:
                    try:
                        with contextlib.suppress(OSError):
                            signal.pidfd_send_signal(fd, sig)
                    finally:
                        os.close(fd)
            with contextlib.suppress(ChildProcessError):
                os.waitpid(pid, os.WNOHANG)
        if time.monotonic() > deadline - seconds / 2:
            escalated = True
        time.sleep(.05)
    return adopted_children(leader)


def wait_exit(pidfd, seconds):
    return bool(select.select([pidfd], [], [], seconds)[0])


def reap(child, seconds=10):
    """Bounded reap: None if the child cannot be reaped in time (for example
    uninterruptible sleep even after SIGKILL); the caller records it."""
    try:
        return child.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        return None


def stop_child(child, pidfd, group):
    """Bounded stop of the unreaped child (and its group). Returns its status
    and any adopted descendants that survived the bounded cleanup."""
    for signum, seconds in ((signal.SIGTERM, 5), (signal.SIGKILL, 5)):
        if group:
            if not group_members(child.pid):
                break
            with contextlib.suppress(ProcessLookupError):
                # The unreaped leader keeps this PGID from being reused.
                os.killpg(child.pid, signum)
            until = time.monotonic() + seconds
            while group_members(child.pid) and time.monotonic() < until:
                time.sleep(.05)
        else:
            if wait_exit(pidfd, 0):
                break
            signal.pidfd_send_signal(pidfd, signum)
            if wait_exit(pidfd, seconds):
                break
    survivors = stop_adopted(child.pid, 10) if group else []
    code = reap(child)
    if code is None:
        survivors = [child.pid, *survivors]
        code = -signal.SIGKILL
    return code, survivors


def write_status(prefix, state, code):
    status = prefix.with_name(prefix.name + ".status")
    temporary = status.with_name(status.name + ".tmp")
    temporary.write_text(json.dumps({"state": state, "code": code}) + "\n")
    temporary.replace(status)


def unblock_in_child():
    signal.pthread_sigmask(signal.SIG_UNBLOCK, STOP_SIGNALS)


def main():
    argv = sys.argv[1:]
    group = argv[:1] == ["--group"]
    if group:
        argv = argv[1:]
    if len(argv) < 4 or argv[2] != "--":
        print(__doc__, file=sys.stderr)
        return 64
    if argv[0] != "-":
        print(__doc__, file=sys.stderr)
        return 64
    prefix, command = Path(argv[1]), argv[3:]
    # Do not keep inherited control descriptors (including the shell's write
    # end of this FIFO) open after the shell has gone.
    os.closerange(3, 65536)
    for signum in STOP_SIGNALS:
        signal.signal(signum, request_stop)
    # Defer stop requests until the child is created, owned and published.
    signal.pthread_sigmask(signal.SIG_BLOCK, STOP_SIGNALS)
    child, pidfd, control = None, -1, 0  # Standard input is the control FIFO.
    stopped, failure = False, None
    try:
        if group:
            import ctypes
            libc = ctypes.CDLL(None, use_errno=True)
            if libc.prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0:
                raise OSError("cannot become a child subreaper")
        child = subprocess.Popen(command, stdin=subprocess.DEVNULL, close_fds=True,
                                 start_new_session=group, preexec_fn=unblock_in_child)
        injected = os.environ.get("OWNED_TEST_FAIL_PIDFD")
        if injected in ("1", "all"):
            # Test-only fault injection: let descendants start, then fail this
            # ("1") or every later ("all") pidfd acquisition.
            time.sleep(1)
            if injected == "all":
                def unavailable(*_):
                    raise OSError("injected persistent pidfd failure")
                os.pidfd_open = unavailable
            raise OSError("injected pidfd acquisition failure")
        pidfd = os.pidfd_open(child.pid)
        temporary = prefix.with_name(prefix.name + ".pid.tmp")
        temporary.write_text(f"{child.pid}\n")
        temporary.replace(prefix.with_name(prefix.name + ".pid"))
    except BaseException as error:  # noqa: BLE001 -- recorded, then cleaned up below
        failure = error
    try:
        signal.pthread_sigmask(signal.SIG_UNBLOCK, STOP_SIGNALS)  # Deferred stops arrive here.
        while failure is None:
            ready, _, _ = select.select([control, pidfd], [], [])
            if pidfd in ready:
                break  # Exited by itself; still unreaped, so the IDs are reserved.
            os.read(control, 64)  # Any message or EOF requests a stop.
            stopped = True
            break
    except StopRequest:
        stopped = True
    except BaseException as error:  # noqa: BLE001
        failure = failure or error
    finally:
        for signum in STOP_SIGNALS:
            signal.signal(signum, signal.SIG_IGN)
        code = 70
        survivors = []
        if child is not None:
            if pidfd < 0:
                # pidfd acquisition failed: the child is still unreaped, so a
                # PID-based stop of our own child (and its group) is safe here.
                with contextlib.suppress(ProcessLookupError):
                    (os.killpg if group else os.kill)(child.pid, signal.SIGKILL)
                unreaped = [child.pid] if reap(child) is None else []
                survivors = unreaped + (stop_adopted(child.pid, 10) if group else [])
            else:
                code, survivors = stop_child(child, pidfd, group)
                if code < 0:
                    code = 0 if stopped and -code == signal.SIGTERM else 128 - code
        if survivors and failure is None:
            # Never report a clean stop while a descendant remains.
            failure = RuntimeError(f"descendants survived bounded cleanup: {survivors}")
        if pidfd >= 0:
            os.close(pidfd)
        os.close(control)
        if failure is not None:
            print(f"owned.py: {type(failure).__name__}: {failure}", file=sys.stderr)
            code = 70
        state = "failed" if failure is not None else "stopped" if stopped else "exited"
        with contextlib.suppress(OSError):
            write_status(prefix, state, code)
    return code


if __name__ == "__main__":
    sys.exit(main())
