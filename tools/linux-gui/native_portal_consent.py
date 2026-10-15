"""Real native GNOME portal consent UI; test-only, private namespace required."""

import json
import os
import subprocess
import sys
import time
from collections import deque
from contextlib import suppress
from pathlib import Path

# This helper may synthesize input only in the runner's owned compositor.
if (
    os.environ.get("LAYERC_TEST_PRIVATE_NATIVE_SESSION") != "1"
    or os.environ.get("DISPLAY")
    or os.environ.get("AT_SPI_BUS_ADDRESS")
    or "/layerc-consent." not in os.environ.get("XDG_RUNTIME_DIR", "")
):
    raise SystemExit("native consent UI requires the runner-owned private session")
import gi

if len(sys.argv) > 1 and sys.argv[1] == "--fixture":
    gi.require_version("Gtk", "4.0")
    from gi.repository import Gtk

    app = Gtk.Application(application_id="io.github.EnderKeera.LayerC.ConsentFixture")

    def activate(app):
        window = Gtk.ApplicationWindow(application=app, title="LayerC consent fixture")
        window.set_default_size(560, 320)
        window.set_child(Gtk.Label(label="Disposable native Wayland capture target"))
        window.present()

    app.connect("activate", activate)
    app.run([])
    raise SystemExit

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi, Gio, GLib

Atspi.set_timeout(800, 800)
root = Path(sys.argv[1])
case = sys.argv[2]
actions_log = []


def record(operation, **details):
    actions_log.append(dict(operation=operation, **details))
    (root / "actions.json").write_text(
        json.dumps(actions_log, ensure_ascii=False, indent=2)
    )


def collect(in_shell=False):
    desktop = Atspi.get_desktop(0)
    queue = deque()
    for i in range(desktop.get_child_count()):
        app = desktop.get_child_at_index(i)
        if (app.get_name() == "gnome-shell") == in_shell:
            queue.append((app, 0))
    nodes = []
    while queue and len(nodes) < 2000:
        node, depth = queue.popleft()
        if depth > 26:
            continue
        try:
            iface = node.get_action_iface()
            names = (
                []
                if iface is None
                else [iface.get_action_name(i) for i in range(iface.get_n_actions())]
            )
            row = {
                "name": node.get_name(),
                "role": node.get_role_name(),
                "depth": depth,
                "actions": names,
                "showing": node.get_state_set().contains(Atspi.StateType.SHOWING),
            }
            nodes.append((node, row))
            for i in range(node.get_child_count()):
                queue.append((node.get_child_at_index(i), depth + 1))
        except (GLib.Error, RuntimeError, AttributeError) as error:
            record("a11y-node-unavailable", error=str(error))
    return nodes


def snapshot(name, in_shell=False):
    nodes = collect(in_shell)
    rows = [row for _, row in nodes if row["name"] or row["actions"]]
    (root / (name + ".json")).write_text(json.dumps(rows, ensure_ascii=False, indent=2))
    return nodes


def wait_for(predicate, description, seconds=12):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        result = predicate()
        if result:
            return result
        time.sleep(0.15)
    raise AssertionError(description)


def picker():
    return any(
        row["name"] == "Share Screen" and row["role"] == "frame" and row["showing"]
        for _, row in collect()
    )


def screenshot(name):
    path = root / (name + ".png")
    reply = subprocess.run(
        [
            "gdbus",
            "call",
            "--session",
            "--dest",
            "org.gnome.Shell.Screenshot",
            "--object-path",
            "/org/gnome/Shell/Screenshot",
            "--method",
            "org.gnome.Shell.Screenshot.Screenshot",
            "false",
            "false",
            str(path),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    assert path.is_file() and path.stat().st_size > 100, reply.stdout
    record("native-compositor-screenshot", file=path.name)


def click_named(name, in_shell=False):
    nodes = collect(in_shell)
    node, row = next(
        (node, row)
        for node, row in nodes
        if row["name"].replace("_", "") == name and row["showing"] and row["actions"]
    )
    iface = node.get_action_iface()
    assert iface.do_action(0), name + " action failed"
    record("native-atspi-action", name=name, role=row["role"], action=row["actions"][0])


def native_pointer_click(node, name, position=None):
    # Test-only virtual input in this owned compositor. Nothing is linked into
    # LayerD/LayerE, and the session's private D-Bus cannot reach a user desktop.
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    destination = "org.gnome.Mutter.RemoteDesktop"
    interface = destination + ".Session"
    reply = bus.call_sync(
        destination,
        "/org/gnome/Mutter/RemoteDesktop",
        destination,
        "CreateSession",
        None,
        GLib.VariantType.new("(o)"),
        Gio.DBusCallFlags.NONE,
        5000,
        None,
    )
    path = reply.unpack()[0]

    def call(method, args=None):
        return bus.call_sync(
            destination,
            path,
            interface,
            method,
            args,
            None,
            Gio.DBusCallFlags.NONE,
            5000,
            None,
        )

    try:
        call("Start")
        # Starting test input changes the Shell sharing indicator. Resolve its
        # bounds only after that layout has settled, never from a stale rect.
        time.sleep(0.4)
        rect = node.get_component_iface().get_extents(Atspi.CoordType.SCREEN)
        x, y = position or (rect.x + rect.width / 2, rect.y + rect.height / 2)
        assert rect.width > 0 and rect.height > 0 and 0 <= x < 1280 and 0 <= y < 800
        record("pointer-target-after-input-start", name=name, x=x, y=y)
        # Home at the bottom-left edge: the top-left is GNOME's hot corner, and
        # touching it opens the Overview under the pending click.
        call("NotifyPointerMotionRelative", GLib.Variant("(dd)", (-10000.0, 10000.0)))
        time.sleep(0.05)
        call("NotifyPointerMotionRelative", GLib.Variant("(dd)", (x, y - 799.0)))
        time.sleep(0.1)
        call("NotifyPointerButton", GLib.Variant("(ib)", (272, True)))
        time.sleep(0.05)
        # A real Stop Screen Sharing press also stops this test input session;
        # its destruction releases any remaining input state.
        with suppress(GLib.Error):
            call("NotifyPointerButton", GLib.Variant("(ib)", (272, False)))
        record("private-compositor-pointer-click", name=name, x=x, y=y)
    finally:
        with suppress(GLib.Error):
            call("Stop")


def client_has(fragment):
    return fragment in (root / "client.log").read_text()


try:
    wait_for(picker, "real GNOME source picker never appeared")
    snapshot("picker-a11y")
    screenshot("picker")
    if case == "deny":
        click_named("Cancel")
        wait_for(
            lambda: client_has('"reason":"portal_denied"'),
            "real denial did not reach product",
        )
        wait_for(lambda: not picker(), "denied picker remained visible")
        screenshot("denied")
        observed = "native Cancel button -> portal_denied, picker dismissed"
    elif case == "cancel":
        (root / "cancel").touch()
        record("client-stop", file="cancel")
        wait_for(
            lambda: client_has('"clientCancelled":true'),
            "client stop was not acknowledged",
        )
        wait_for(lambda: not picker(), "cancelled native picker remained visible")
        screenshot("cancelled")
        observed = "client stop -> native request dismissed without granting capture"
    elif case == "revoke":
        nodes = collect()
        node, row = next(
            (node, row)
            for node, row in nodes
            if row["name"] == "LayerC consent fixture" and row["role"] == "list item"
        )
        parent = node.get_parent()
        selection = parent.get_selection_iface()
        index = next(
            i
            for i in range(parent.get_child_count())
            if parent.get_child_at_index(i) == node
        )
        if selection is not None and selection.select_child(index):
            record("native-atspi-select", name=row["name"], index=index)
        else:
            # GTK4 does not implement Selection or focus for this row and
            # exposes only local (0,0) screen geometry on Wayland. This pixel
            # is grounded in the recorded 1280x800 native picker screenshot.
            record(
                "native-picker-visual-fallback",
                screenshot="picker.png",
                resolution=[1280, 800],
            )
            native_pointer_click(node, row["name"], position=(640.0, 354.0))
        time.sleep(0.3)
        screenshot("selected")
        click_named("Share")
        wait_for(
            lambda: client_has('"ready":true'),
            "grant did not return real PipeWire remote",
            seconds=20,
        )
        screenshot("granted")
        snapshot("shell-controls", in_shell=True)
        stop = next(
            node
            for node, row in collect(in_shell=True)
            if row["name"] == "Stop Screen Sharing" and row["showing"]
        )
        native_pointer_click(stop, "Stop Screen Sharing")
        screenshot("stop-requested")
        snapshot("stop-requested-a11y", in_shell=True)
        wait_for(
            lambda: client_has('"reason":"portal_session_closed"'),
            "native revoke did not reach product",
        )
        screenshot("revoked")
        observed = "native source selected and granted -> Shell Stop Screen Sharing -> portal_session_closed"
    else:
        raise AssertionError("unknown case")
    (root / "result.json").write_text(
        json.dumps({"case": case, "passed": True, "observed": observed}, indent=2)
    )
    print(observed, flush=True)
finally:
    (root / "finish").touch()
