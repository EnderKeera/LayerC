#!/usr/bin/env python3
"""Local computer-use bridge for a disposable Linux LayerE X11 session.

`serve` runs inside the session owned by tools/linux-editor-gui-tests.sh. It
listens on a private Unix socket; each request must come from the same user
and present the session token. Input is synthesized only into the runner's
private Xvfb, never a user desktop:

- The display must be served by the runner-owned Xvfb (lock file, command
  line and start time), held through a pidfd for the bridge's lifetime.
- A random nonce is set on that server's root window. Before every X client
  command (each screenshot and each xdotool step of an action) the bridge
  confirms the Xvfb is alive and the nonce is present, and rechecks after an
  action. Losing the display ends the session. A residual race remains only
  between a confirmation and that command's own connect.
- Every coordinate/keyboard action must cite the latest screenshot, taken
  after the previous action. Freshness is consumed before the first input is
  sent, so a partially executed action can never be repeated blind.

`call` sends one JSON request and prints the JSON response. The Windows-side
runner (tools/test-linux-gui.ps1 -Mode Call) and the scripted walkthrough use
the same protocol, so an agent and the walkthrough exercise identical paths.
"""
import argparse
import hashlib
import json
import os
import re
import secrets
import select
import socket
import stat
import struct
import subprocess
import sys
import time
from pathlib import Path

MAX_REQUEST = 64 * 1024
RECEIVE_SECONDS = 10.0    # Whole request must arrive within this window.
OPERATION_SECONDS = 60.0  # Every subprocess of one operation shares this budget.
SEND_SECONDS = 5.0
MAX_TEXT = 256
TYPE_DELAY_MS = 20
INPUT_OPS = {"click", "drag", "set_text", "key", "type", "activate", "a11y_check"}
FIXTURE_OPS = {"block_profile", "restore_profile", "daemon_stop", "daemon_start"}
KEY_PATTERN = re.compile(r"^[A-Za-z0-9_+]{1,64}$")
NAME_PATTERN = re.compile(r"[^a-z0-9-]+")
NONCE_PROPERTY = "_LAYERC_GUI_SESSION"
SETTLE_SECONDS = 0.25


class Rejected(Exception):
    """A request the bridge refuses; reported to the caller and logged."""


class DisplayLost(Exception):
    """The runner-owned display can no longer be proven; the session ends."""


def run(command, timeout=10.0, check=True):
    result = subprocess.run(command, capture_output=True, text=True,
                            timeout=timeout, check=False)
    if check and result.returncode != 0:
        raise RuntimeError(f"{command[0]} failed ({result.returncode}): "
                           f"{result.stderr.strip()[:300]}")
    return result.stdout


def private_directory(path):
    status = os.lstat(path)
    if (not stat.S_ISDIR(status.st_mode) or status.st_uid != os.geteuid()
            or status.st_mode & 0o077):
        raise SystemExit(f"session directory is not private: {path}")


def integer(request, key, low, high):
    value = request.get(key)
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        raise Rejected(f"{key} must be an integer in [{low}, {high}]")
    return value


def start_time(pid):
    return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19]


class Bridge:
    def __init__(self, args):
        self.session = Path(args.session)
        self.evidence = Path(args.evidence)
        self.shots = self.evidence / "shots"
        self.shots.mkdir(parents=True, exist_ok=True)
        self.actions = (self.evidence / "actions.jsonl").open("a", buffering=1)
        self.profiles = Path(args.profile_dir)
        self.aside = self.session / "aside"
        self.layerctl = args.layerctl
        self.daemon_control = Path(args.daemon_control)
        # Private import/export directory for file-chooser cases.
        self.exchange_dir = self.session / "exchange"
        self.exchange_dir.mkdir(mode=0o700, exist_ok=True)
        # A file name that is not UTF-8: the chooser lists it while AT-SPI
        # reads the tree, which must not reach D-Bus as invalid UTF-8.
        (self.exchange_dir / os.fsdecode(b"\xff-not-utf8.json")).write_bytes(b"")
        self.idle = args.idle_seconds
        self.sequence = 0          # Actions begun; each consumes freshness.
        self.shot_index = 0
        self.latest = None         # {"id", "sequence"}
        self.token = secrets.token_hex(32)
        self.deadline = time.monotonic() + OPERATION_SECONDS
        self.display_number = self.pin_display(args.display, args.xvfb_pid)
        self.nonce = secrets.token_hex(16)
        run(["xprop", "-root", "-f", NONCE_PROPERTY, "8s", "-set", NONCE_PROPERTY, self.nonce])
        self.confirm_display()
        dims = re.search(r"dimensions:\s+(\d+)x(\d+)", run(["xdpyinfo"]))
        if not dims:
            raise SystemExit("cannot read the private display dimensions")
        self.width, self.height = int(dims.group(1)), int(dims.group(2))

    def pin_display(self, display, xvfb_pid):
        # Refuse anything but the runner's own private Xvfb: a user display
        # must never receive synthesized input.
        match = re.fullmatch(r":(\d+)", display or "")
        if os.environ.get("DISPLAY") != display or not match or int(match.group(1)) < 1000:
            raise SystemExit("bridge requires the runner-owned private display")
        number = match.group(1)
        before = start_time(xvfb_pid)
        self.xvfb = os.pidfd_open(xvfb_pid)
        # The pidfd now names one process; re-verify that it is the same,
        # still-running Xvfb that owns the display lock.
        cmdline = Path(f"/proc/{xvfb_pid}/cmdline").read_bytes().split(b"\0")
        if (select.select([self.xvfb], [], [], 0)[0] or start_time(xvfb_pid) != before
                or not cmdline[0].endswith(b"Xvfb")
                or Path(f"/tmp/.X{number}-lock").read_text().strip() != str(xvfb_pid)):
            raise SystemExit("display is not served by the runner-owned Xvfb")
        return number

    def remaining(self, cap):
        left = self.deadline - time.monotonic()
        if left <= 0:
            raise RuntimeError("operation deadline exceeded")
        return min(cap, left)

    def confirm_display(self):
        if select.select([self.xvfb], [], [], 0)[0]:
            raise DisplayLost("the runner-owned Xvfb exited")
        value = run(["xprop", "-root", NONCE_PROPERTY], timeout=self.remaining(5.0), check=False)
        if f'"{self.nonce}"' not in value:
            raise DisplayLost("the display no longer carries this session's nonce")

    def log(self, request, kind, ok, detail):
        args = ({k: v for k, v in request.items() if k not in ("op", "token")}
                if isinstance(request, dict) else {"unparsed": True})
        entry = {"monotonic": round(time.monotonic(), 6), "sequence": self.sequence,
                 "op": request.get("op") if isinstance(request, dict) else None,
                 "kind": kind, "ok": ok, "args": args}
        entry.update(detail)
        self.actions.write(json.dumps(entry, sort_keys=True, default=str) + "\n")

    # Observation ---------------------------------------------------------
    def windows(self):
        found = []

        def query(command, check=True):  # Confirmed and deadline-bounded, like input.
            return self.bounded(command, check=check)

        ids = query(["xdotool", "search", "--onlyvisible", "--name", "."], check=False)
        for wid in ids.split():
            try:
                name = query(["xdotool", "getwindowname", wid]).strip()
                # xwininfo reports root coordinates; xdotool's geometry is
                # parent-relative once a window manager reparents the client.
                info = query(["xwininfo", "-id", wid])
                geometry = {}
                for key, pattern in (("x", r"Absolute upper-left X:\s+(-?\d+)"),
                                     ("y", r"Absolute upper-left Y:\s+(-?\d+)"),
                                     ("width", r"\bWidth:\s+(\d+)"),
                                     ("height", r"\bHeight:\s+(\d+)")):
                    match = re.search(pattern, info)
                    if not match:
                        raise ValueError("incomplete xwininfo output")
                    geometry[key] = int(match.group(1))
                pid = query(["xdotool", "getwindowpid", wid], check=False).strip()
                extents = re.findall(r"\d+", query(
                    ["xprop", "-id", wid, "_NET_FRAME_EXTENTS"], check=False))
            except (ValueError, subprocess.TimeoutExpired):
                continue  # Destroyed while being described; not an error.
            except RuntimeError as error:
                if "deadline" in str(error):
                    raise
                continue
            found.append({"id": int(wid), "name": name, "pid": int(pid) if pid.isdigit() else None,
                          **geometry,
                          "frame": [int(v) for v in extents[:4]] if len(extents) >= 4 else None})
        return found

    def op_screenshot(self, request):
        name = request.get("name", "shot")
        if not isinstance(name, str):
            raise Rejected("name must be a string")
        self.shot_index += 1
        label = NAME_PATTERN.sub("-", name.lower()).strip("-")[:40] or "shot"
        path = self.shots / f"{self.shot_index:03d}-{label}.png"
        self.bounded(["import", "-window", "root", str(path)], timeout=20)
        self.latest = {"id": self.shot_index, "sequence": self.sequence}
        return {"screenshot": self.shot_index, "path": str(path),
                "relative": str(path.relative_to(self.evidence)),
                "screen": [self.width, self.height], "windows": self.windows()}

    def shot_path(self, request):
        wanted = request.get("screenshot")
        if isinstance(wanted, bool) or not isinstance(wanted, int) or wanted < 1:
            raise Rejected("unknown screenshot id")
        candidates = sorted(self.shots.glob(f"{wanted:03d}-*.png"))
        if len(candidates) != 1:
            raise Rejected("unknown screenshot id")
        return candidates[0]

    def op_inspect(self, request):
        """Pixel facts for a screenshot region: digest, color count, centroid."""
        path = self.shot_path(request)
        region = request.get("region", [0, 0, self.width, self.height])
        if (not isinstance(region, list) or len(region) != 4
                or any(isinstance(v, bool) or not isinstance(v, int) for v in region)):
            raise Rejected("region must be four integers")
        x, y, w, h = region
        if w <= 0 or h <= 0 or x < 0 or y < 0 or x + w > self.width or y + h > self.height:
            raise Rejected("region is outside the screen")
        color = None
        if "rgb" in request:
            rgb = request["rgb"]
            if (not isinstance(rgb, list) or len(rgb) != 3 or any(
                    isinstance(v, bool) or not isinstance(v, int) or not 0 <= v <= 255 for v in rgb)):
                raise Rejected("rgb must be three integers in [0, 255]")
            color = bytes(rgb)
        raw = subprocess.run(["convert", str(path), "-crop", f"{w}x{h}+{x}+{y}", "+repage",
                              "-depth", "8", "rgb:-"], capture_output=True,
                             timeout=self.remaining(20.0), check=True).stdout
        if len(raw) != w * h * 3:
            raise RuntimeError("unexpected decoded region size")
        result: dict[str, object] = {"sha256": hashlib.sha256(raw).hexdigest()}
        if color is not None:
            count = sx = sy = 0
            for offset in range(0, len(raw), 3):
                if raw[offset:offset + 3] == color:
                    pixel = offset // 3
                    count += 1
                    sx += pixel % w
                    sy += pixel // w
            result.update(count=count, meanX=x + sx / count if count else None,
                          meanY=y + sy / count if count else None)
        return result

    def op_state(self, _request):
        profiles = []
        for entry in sorted(self.profiles.iterdir()):
            item: dict[str, object] = {"name": entry.name,
                                       "kind": "directory" if entry.is_dir() else "file"}
            if entry.is_file() and not entry.is_symlink():
                data = entry.read_bytes()
                item["sha256"] = hashlib.sha256(data).hexdigest()
                try:
                    item["json"] = json.loads(data)
                except ValueError:
                    item["json"] = None
            profiles.append(item)
        status = subprocess.run([self.layerctl, "status", "--json"], capture_output=True,
                                text=True, timeout=self.remaining(10.0), check=False)
        try:
            daemon = json.loads(status.stdout) if status.returncode == 0 else None
        except ValueError:
            daemon = None
        exchange = {entry.name: hashlib.sha256(entry.read_bytes()).hexdigest()
                    for entry in sorted(self.exchange_dir.iterdir())
                    if entry.is_file() and not entry.is_symlink()}
        return {"profiles": profiles, "exchange": str(self.exchange_dir), "exchangeFiles": exchange,
                "daemon": {"reachable": daemon is not None, "status": daemon,
                           "error": None if daemon else status.stderr.strip()[:300]}}

    def op_tree(self, _request):
        # Out of process: an absent or wedged accessibility bus cannot stall us.
        script = Path(__file__).with_name("a11y.py")
        try:
            return json.loads(run([sys.executable, str(script)], timeout=self.remaining(10.0)))
        except (RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
            return {"available": False, "reason": f"accessibility query failed: {error}"}

    def op_a11y_check(self, _request):
        # Semantic walkthrough through AT-SPI; it renames and saves the fixture
        # and sends one Tab key, so it is an input operation.
        self.begin()
        script = Path(__file__).with_name("a11y_check.py")
        output = self.bounded([sys.executable, str(script), str(self.profiles)],
                              timeout=50.0)
        try:
            return json.loads(output)
        except ValueError as error:
            raise Rejected(f"accessibility check produced no report: {error}") from error

    # Actions -------------------------------------------------------------
    # Each op validates every argument first, then calls begin() immediately
    # before its first X request; begin() consumes screenshot freshness.
    def bounded(self, command, timeout=10.0, confirm=True, check=True):
        """Run one subprocess within the operation's shared deadline,
        confirming the pinned display first when it is an X client."""
        if confirm:
            self.confirm_display()
        return run(command, timeout=self.remaining(timeout), check=check)

    def require_fresh(self, request):
        latest = self.latest
        if latest is None or request.get("screenshot") != latest["id"]:
            raise Rejected("action must cite the latest screenshot id")
        if latest["sequence"] != self.sequence:
            raise Rejected("screenshot predates the previous action; capture again")

    def begin(self):
        self.confirm_display()
        self.sequence += 1

    def point(self, request, prefix=""):
        return (str(integer(request, prefix + "x", 0, self.width - 1)),
                str(integer(request, prefix + "y", 0, self.height - 1)))

    def text_arg(self, request, allow_empty):
        text = request.get("text")
        if not isinstance(text, str) or len(text) > MAX_TEXT or (not text and not allow_empty):
            raise Rejected(f"text must be a string of at most {MAX_TEXT} characters")
        return text

    def type_text(self, text):
        # "--" keeps leading-dash text literal (never an xdotool option).
        self.bounded(["xdotool", "type", "--clearmodifiers", "--delay", str(TYPE_DELAY_MS),
                      "--", text], timeout=5 + len(text) * TYPE_DELAY_MS / 1000 * 2)

    def window_id(self, request):
        wid = integer(request, "window", 1, 2**32 - 1)
        if wid not in {w["id"] for w in self.windows()}:
            raise Rejected("unknown or invisible window id")
        return str(wid)

    def op_click(self, request):
        x, y = self.point(request)
        button = integer(request, "button", 1, 3) if "button" in request else 1
        count = integer(request, "count", 1, 3) if "count" in request else 1
        self.begin()
        self.bounded(["xdotool", "mousemove", x, y, "click", "--repeat", str(count), str(button)])

    def op_drag(self, request):
        x1, y1 = (int(v) for v in self.point(request))
        x2, y2 = (int(v) for v in self.point(request, "to_"))
        button = str(integer(request, "button", 1, 3) if "button" in request else 1)
        self.begin()
        try:  # Protected from before the press: a button is never left held.
            self.bounded(["xdotool", "mousemove", str(x1), str(y1), "mousedown", button])
            for step in range(1, 13):  # Incremental motion, as a pointer drag.
                self.bounded(["xdotool", "mousemove", str(x1 + (x2 - x1) * step // 12),
                              str(y1 + (y2 - y1) * step // 12)])
                time.sleep(0.03)
        finally:
            # A reserved grace budget for the release, on a re-confirmed
            # display. If release cannot be established, end the session.
            self.deadline = max(self.deadline, time.monotonic() + 5.0)
            try:
                self.bounded(["xdotool", "mouseup", button], timeout=5.0)
            except (RuntimeError, subprocess.SubprocessError) as error:
                raise DisplayLost(f"could not release the drag button: {error}") from error

    def op_set_text(self, request):
        x, y = self.point(request)
        text = self.text_arg(request, allow_empty=True)
        self.begin()
        self.bounded(["xdotool", "mousemove", x, y, "click", "1", "key", "--clearmodifiers", "ctrl+a"])
        if text:
            self.type_text(text)
        else:
            self.bounded(["xdotool", "key", "--clearmodifiers", "BackSpace"])

    def op_key(self, request):
        combo = request.get("combo")
        if not isinstance(combo, str) or not KEY_PATTERN.match(combo):
            raise Rejected("invalid key combination")
        self.begin()
        self.bounded(["xdotool", "key", "--clearmodifiers", combo])

    def op_type(self, request):
        text = self.text_arg(request, allow_empty=False)
        self.begin()
        self.type_text(text)

    def op_activate(self, request):
        wid = self.window_id(request)
        self.begin()
        self.bounded(["xdotool", "windowactivate", "--sync", wid, "windowfocus", "--sync", wid])

    def op_set_geometry(self, request):
        # Direct geometry is a repeatable layout tool, logged separately from
        # user resize drags; it does not prove window-manager resizing.
        wid = self.window_id(request)
        values = [integer(request, k, 0, 16384) for k in ("x", "y", "width", "height")]
        self.begin()
        self.bounded(["xdotool", "windowmove", wid, str(values[0]), str(values[1]),
                      "windowsize", wid, str(values[2]), str(values[3])])

    # Test-only fixture faults ------------------------------------------------
    def profile_entry(self, request):
        name = request.get("file")
        if not isinstance(name, str) or "/" in name or "\0" in name or name in ("", ".", ".."):
            raise Rejected("file must name an entry in the private profile store")
        return self.profiles / name, self.aside / name

    def op_block_profile(self, request):
        path, aside = self.profile_entry(request)
        if not path.is_file() or path.is_symlink() or aside.exists():
            raise Rejected("profile is not a regular file, or is already blocked")
        self.sequence += 1
        self.aside.mkdir(mode=0o700, exist_ok=True)
        path.rename(aside)
        path.mkdir(mode=0o700)  # The store cannot replace a directory.

    def op_restore_profile(self, request):
        path, aside = self.profile_entry(request)
        if not aside.is_file() or not path.is_dir():
            raise Rejected("profile is not blocked")
        self.sequence += 1
        path.rmdir()
        aside.rename(path)

    def daemon_reachable(self):
        return subprocess.run([self.layerctl, "status", "--json"], capture_output=True,
                              timeout=self.remaining(10.0), check=False).returncode == 0

    def op_daemon_start(self, _request):
        # The runner keeps LayerD under an owned restart loop that waits on
        # this FIFO after a stop; the bridge never spawns the daemon itself.
        if self.daemon_reachable():
            raise Rejected("LayerD is already running")
        self.sequence += 1
        while True:
            try:
                descriptor = os.open(self.daemon_control, os.O_WRONLY | os.O_NONBLOCK)
                break
            except OSError:  # ENXIO until the loop is waiting; remaining() bounds it.
                self.remaining(1.0)
                time.sleep(0.05)
        with os.fdopen(descriptor, "w") as control:
            control.write("start\n")
        while not self.daemon_reachable():
            self.remaining(1.0)
            time.sleep(0.1)
        return {"started": True}

    def op_daemon_stop(self, _request):
        self.sequence += 1
        run([self.layerctl, "stop"], timeout=self.remaining(10.0))
        while True:
            if subprocess.run([self.layerctl, "status", "--json"], capture_output=True,
                              timeout=self.remaining(10.0), check=False).returncode != 0:
                return {"stopped": True}
            time.sleep(0.1)  # remaining() ends the loop at the operation deadline.

    # Protocol --------------------------------------------------------------
    def handle(self, request):
        op = request.get("op")
        handler = getattr(self, f"op_{op}", None) if isinstance(op, str) else None
        if handler is None or op == "stop":
            raise Rejected("unknown operation")
        kind = ("input" if op in INPUT_OPS else "direct-geometry" if op == "set_geometry"
                else "fixture" if op in FIXTURE_OPS else "observe")
        # Every check below runs within this operation's own budget, never a
        # previous operation's expired deadline.
        self.deadline = time.monotonic() + OPERATION_SECONDS
        if kind in ("input", "direct-geometry"):
            self.require_fresh(request)
        elif op == "screenshot":
            self.confirm_display()
        before = self.sequence
        try:
            result = handler(request)
        finally:
            if self.sequence != before:
                time.sleep(SETTLE_SECONDS)  # Let the client and WM repaint, even after a failure.
                if kind in ("input", "direct-geometry"):
                    self.deadline = max(self.deadline, time.monotonic() + 5.0)
                    self.confirm_display()
        return kind, result or {}

    def serve(self):
        # Publish the token before the socket, so a visible socket is ready.
        token_file = self.session / "bridge.token"
        descriptor = os.open(token_file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w") as handle:
            handle.write(self.token + "\n")
        path = self.session / "bridge.sock"
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        old = os.umask(0o177)
        try:
            listener.bind(str(path))
        finally:
            os.umask(old)
        listener.listen(4)
        print("bridge-ready", flush=True)
        reason = "stopped"
        idle_until = time.monotonic() + self.idle
        try:
            while True:
                # Wake for a client, the owned Xvfb's exit, or the idle limit.
                ready, _, _ = select.select([listener, self.xvfb], [], [],
                                            max(0.0, idle_until - time.monotonic()))
                if self.xvfb in ready:
                    reason = "display-lost"
                    break
                if not ready:
                    reason = "idle-timeout"
                    break
                connection, _ = listener.accept()
                idle_until = time.monotonic() + self.idle
                with connection:
                    outcome = self.exchange(connection)
                if outcome:
                    reason = outcome
                    break
        finally:
            listener.close()
            path.unlink(missing_ok=True)
            token_file.unlink(missing_ok=True)
            self.log({"op": "bridge-exit"}, "lifecycle", True, {"reason": reason})
            self.actions.close()
        return 0 if reason in ("stopped", "idle-timeout") else 75

    def receive(self, connection, deadline):
        data = b""
        while not data.endswith(b"\n"):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise Rejected("request not received within the receive deadline")
            connection.settimeout(remaining)
            chunk = connection.recv(4096)
            if not chunk:
                break
            data += chunk
            if len(data) > MAX_REQUEST:
                raise Rejected("request too large")
        return data

    def exchange(self, connection):
        """Serve one request. Returns a reason string when the bridge must exit."""
        deadline = time.monotonic() + RECEIVE_SECONDS
        request, ending = None, None
        try:
            creds = connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED,
                                          struct.calcsize("3i"))
            if struct.unpack("3i", creds)[1] != os.geteuid():
                raise Rejected("peer is not the session owner")
            request = json.loads(self.receive(connection, deadline))
            if not isinstance(request, dict):
                raise Rejected("request must be a JSON object")
            token = request.get("token")
            if not isinstance(token, str) or not token.isascii() or \
                    not secrets.compare_digest(token, self.token):
                raise Rejected("invalid session token")
            if request.get("op") == "stop":
                kind, result, ending = "lifecycle", {}, "stopped"
            else:
                kind, result = self.handle(request)
            response = {"ok": True, **result}
            self.log(request, kind, True, {k: v for k, v in result.items()
                                           if k in ("screenshot", "relative", "count",
                                                    "meanX", "sha256", "stopped")})
        except Rejected as error:
            response = {"ok": False, "rejected": True, "error": str(error)}
            self.log(request, "rejected", False, {"error": str(error)})
        except DisplayLost as error:
            response = {"ok": False, "error": f"display lost: {error}; session ending"}
            self.log(request, "display-lost", False, {"error": str(error)})
            ending = "display-lost"
        except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as error:
            response = {"ok": False, "error": f"{type(error).__name__}: {error}"}
            self.log(request, "error", False, {"error": response["error"]})
        try:
            connection.settimeout(SEND_SECONDS)
            connection.sendall((json.dumps(response, allow_nan=False) + "\n").encode())
        except (OSError, ValueError):
            pass  # The client went away; the action log still records the outcome.
        return ending


def call(session, request, token=None):
    """One request/response exchange; returns the decoded response."""
    session = Path(session)
    if token is None:
        token = (session / "bridge.token").read_text().strip()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        # The bridge serves one request at a time: allow a queued request
        # behind one full operation, plus its own receive/operation/send.
        client.settimeout(2 * OPERATION_SECONDS + RECEIVE_SECONDS + SEND_SECONDS + 10)
        client.connect(str(session / "bridge.sock"))
        client.sendall((json.dumps({**request, "token": token}) + "\n").encode())
        data = b""
        while not data.endswith(b"\n"):
            chunk = client.recv(65536)
            if not chunk:
                break
            data += chunk
    response = json.loads(data)
    if not isinstance(response, dict):
        raise ValueError("bridge response is not a JSON object")
    return response


def main():
    parser = argparse.ArgumentParser(description="LayerE disposable-session bridge")
    commands = parser.add_subparsers(dest="command", required=True)
    serve = commands.add_parser("serve")
    for name in ("--session", "--evidence", "--display", "--profile-dir", "--layerctl",
                 "--daemon-control"):
        serve.add_argument(name, required=True)
    serve.add_argument("--xvfb-pid", required=True, type=int)
    serve.add_argument("--idle-seconds", type=float, default=1800)
    client = commands.add_parser("call")
    client.add_argument("--session", required=True)
    client.add_argument("request", help="JSON object, or @FILE containing one")
    args = parser.parse_args()
    if args.command == "serve":
        private_directory(args.session)
        return Bridge(args).serve()
    raw = args.request
    if raw.startswith("@"):  # Avoids shell/WSL quoting of JSON arguments.
        raw = Path(raw[1:]).read_text(encoding="utf-8")
    response = call(args.session, json.loads(raw))
    print(json.dumps(response, indent=2))
    return 0 if response.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
