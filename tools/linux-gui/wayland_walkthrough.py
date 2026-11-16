#!/usr/bin/env python3
"""Scripted LayerE walkthrough under a private headless sway with XWayland.

The editor draws through X11, so on Wayland it is an XWayland client beside
the XWayland game fixture and a native foot window. Every case compares the
editor's text with LayerD's own status: the display session, compositor and
provider rows must be reported as LayerD reports them, and the X11-only
Detected Tasks scan and placement must say they are degraded. Writes
cases.json in walkthrough.py's format; exits nonzero unless every case passes.
"""
import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from walkthrough import GAME, MINIMUM, Walk, check  # noqa: E402

NATIVE = "LayerC Native Window"
DIAGNOSTICS = "Runtime diagnostics"
DAEMON = "LayerD status and capability faults"
TASKS = "Detected Tasks — LayerE"
SCOPE = "Wayland session: only XWayland windows are listed (degraded)"
PROVIDERS = ("windows", "monitors", "input", "overlay", "capture")

EXPECTED = {
    "wayland-startup": "LayerE and the game fixture run as XWayland clients of headless sway; "
                       "the native foot window is not an X11 window; the AT-SPI bridge is active.",
    "wayland-status": "LayerD reports a Wayland session on wlroots with XWayland window "
                      "tracking degraded; Daemon status names Wayland, the compositor and the "
                      "XWayland connection.",
    "wayland-diagnostics": "Runtime diagnostics shows LayerD's display session and every "
                           "provider row exactly as LayerD reports them.",
    "wayland-detected-tasks": "Detected Tasks lists the XWayland game but not the native window, "
                              "and says the scan is XWayland-only (degraded) with LayerD's "
                              "window provider.",
    "wayland-placement": "Position on window starts on the XWayland game and reports the "
                         "degraded Wayland scope with LayerD's window provider.",
}


def provider_text(provider):
    text = f"{provider['name']}: {provider['state']}"
    reasons = ", ".join(provider.get("reasons") or [])
    return f"{text} ({reasons})" if reasons else text


class WaylandWalk(Walk):
    def daemon_display(self):
        daemon = self.state()["daemon"]
        check(daemon["reachable"], "LayerD is not reachable")
        status = daemon["status"]
        payload = status.get("payload", status)
        display = payload.get("display")
        check(isinstance(display, dict), "LayerD reported no display object")
        return payload, display

    def any_text(self, part, nodes):
        return next((n.get("name") or n.get("text") or "" for n in nodes
                     if part in (n.get("name") or "") or part in (n.get("text") or "")), "")

    def case_wayland_startup(self):
        self.capture("wayland-startup")
        self.window(GAME)
        check(not any(w["name"].startswith(NATIVE) for w in self.shot["windows"]),
              "the native foot window is listed as an X11 window")
        width, height = self.editor_size()
        check(width >= MINIMUM[0] and height >= MINIMUM[1], f"editor below minimum: {width}x{height}")
        self.find("label", "Accessibility: AT-SPI bridge active")
        return f"editor {width}x{height} on XWayland; native window absent from X11"

    def case_wayland_status(self):
        payload, display = self.until(lambda _: self.daemon_display_ready(),
                                      "LayerD never reported a session for the game")
        check(display["session"] == "wayland" and display["compositor"] == "wlroots",
              f"unexpected display: {display['session']}/{display['compositor']}")
        windows = display["providers"]["windows"]
        check(windows["name"] == "x11_ewmh" and windows["state"] == "degraded" and
              "xwayland_windows_only" in windows["reasons"],
              f"XWayland window tracking is not reported degraded: {windows}")
        self.focus("LayerE", "wayland-status-focused")
        self.click(self.find("button", "LayerD status"), "wayland-status-refresh")
        text = self.until(lambda n: (t := self.find("text", DAEMON, n).get("text", "")) and
                          "Wayland (wlroots), XWayland connected" in t and t,
                          "Daemon status does not name the Wayland session")
        check(f"{len(payload['sessions'])} session" in text,
              f"Daemon status session count differs from LayerD: {text!r}")
        self.notes["windowsProvider"] = provider_text(windows)
        return text.splitlines()[0][:160]

    def daemon_display_ready(self):
        payload, display = self.daemon_display()
        return (payload, display) if payload.get("sessions") else None

    def case_wayland_diagnostics(self):
        _, display = self.daemon_display()
        self.focus("LayerE", "wayland-diagnostics-focused")
        self.key("alt+v", "wayland-diagnostics-menu")
        for step in range(8):
            self.key("Down", f"wayland-diagnostics-menu-{step}")
        self.key("Return", "wayland-diagnostics-opened")
        text = self.until(lambda n: (t := self.find("text", DIAGNOSTICS, n).get("text", "")) and
                          "Display:" in t and t, "Runtime diagnostics shows no Display section")
        check("Display: wayland session, compositor wlroots" in text,
              "the diagnostics display line differs from LayerD")
        rows = [f"  {name}: {provider_text(display['providers'][name])}" for name in PROVIDERS]
        missing = [row for row in rows if row not in text.splitlines()]
        check(not missing, f"provider rows differ from LayerD: {missing}")
        self.click(self.find("button", "Close"), "wayland-diagnostics-closed")
        return f"{len(rows)} provider rows match LayerD"

    def case_wayland_detected_tasks(self):
        self.focus("LayerE", "wayland-tasks-focused")
        self.key("ctrl+t", "wayland-tasks-opened")
        self.until(lambda n: self.window_open(TASKS, n), "Ctrl+T did not open Detected Tasks")
        nodes = self.nodes()
        tasks = self.find("list", "Detected tasks", nodes)
        start = nodes.index(tasks)
        rows = [n["name"] for n in nodes[start + 1:] if n["depth"] == tasks["depth"] + 1
                and n["role"] == "list item"]
        check(sum(GAME in row for row in rows) == 1, f"the XWayland game is not listed once: {rows!r}")
        check(not any(NATIVE in row for row in rows), "a native Wayland window was listed")
        note = self.any_text(SCOPE, nodes)
        provider = self.notes.get("windowsProvider", "")
        check(note and f"LayerD tracks windows with {provider}." in note,
              f"the degraded scope note is missing or names another provider: {note!r}")
        self.notes["tasksNote"] = note
        return f"{len(rows)} rows; {note[:120]}"

    def case_wayland_placement(self):
        nodes = self.nodes()
        tasks = self.find("list", "Detected tasks", nodes)
        start = nodes.index(tasks)
        rows = [n["name"] for n in nodes[start + 1:] if n["depth"] == tasks["depth"] + 1
                and n["role"] == "list item"]
        index = next(i for i, row in enumerate(rows) if GAME in row)
        x, y, w, _ = tasks["extents"]
        self.act("click", "wayland-tasks-first-row", x=x + w // 2, y=y + 10)
        for step in range(index):
            self.key("Down", f"wayland-tasks-row-down-{step}")
        self.click(self.find("button", "Position on window"), "wayland-position")
        status = self.until(lambda n: (t := self.status_text(n)).startswith(f'Positioning on "{GAME}"')
                            and t, "positioning did not start on the XWayland game")
        check(SCOPE in status and self.notes.get("windowsProvider", "") in status,
              f"positioning does not report the degraded Wayland scope: {status!r}")
        return status[:200]


def main():
    parser = argparse.ArgumentParser(description="Scripted LayerE walkthrough on Wayland")
    parser.add_argument("--session", required=True)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    walk = WaylandWalk(args.session)
    cases, failed = [], None
    for name in EXPECTED:
        record = {"id": name, "required": True, "expected": EXPECTED[name]}
        if failed:
            record.update(status="blocked", observed=f"not run after {failed} failed")
            cases.append(record)
            continue
        first, started = len(walk.shots), time.monotonic()
        try:
            observed = getattr(walk, "case_" + name.replace("-", "_"))()
            record.update(status="pass", observed=observed)
        except Exception as error:  # noqa: BLE001 -- every failure is recorded
            record.update(status="fail", observed=f"{type(error).__name__}: {error}")
            failed = name
        record.update(seconds=round(time.monotonic() - started, 3),
                      screenshots=walk.shots[first:])
        cases.append(record)
        print(f"{record['status']}: {name} — {record['observed']}", flush=True)
    report = {"cases": cases, "deferred": [], "notes": walk.notes, "pass": failed is None}
    (args.out / "cases.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    return 0 if failed is None else 1


if __name__ == "__main__":
    sys.exit(main())
