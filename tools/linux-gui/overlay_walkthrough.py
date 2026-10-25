"""Overlay integration cases on the controlled X11 target: real X input only.

Observations come from the daemon's own override-redirect windows (bridge
`overlays`), LayerD's session diagnostics and screenshots. Click-through is a
real pointer click observed by an xev watcher on the target window. The cases
run last: they resize the target, stop LayerD and toggle the profile, and
restore each before returning.
"""
import math
import time
from typing import Any

from automation_walkthrough import require

GAME = "LayerC Automation Fixture"
EDITOR = "LayerE"
PROFILE = "fixture.json"
PANEL = "main"
# The fixture panel's fill/border, background and text: every overlay pixel.
PANEL_COLORS = {"50bac8", "202a31", "f1f5f7"}
SLACK = 2  # Pixels between the client geometry LayerD tracks and xwininfo's.

EXPECTED = {
    "overlay-focus-changes": "The test binding shows one overlay window, painted and anchored on "
                             "the focused target; focusing LayerE removes it while LayerD keeps "
                             "the session, and refocusing the target restores it in place.",
    "overlay-target-resize": "Corner drags shrink and then restore the target; each time LayerD "
                             "moves the overlay to the exact anchored rectangle of the new "
                             "client area, its anchor point on the target.",
    "overlay-click-through": "Real clicks beside the overlay and on its painted pixels both reach "
                             "the target window (xev), while the overlay stays mapped.",
    "overlay-cleanup": "Disabling the profile in LayerE removes its session and every overlay "
                       "window while LayerD stays reachable; re-enabling restores the overlay; "
                       "stopping LayerD leaves no overlay window behind.",
}


def lround(value):
    """C lround for the non-negative products LayerD rounds."""
    return math.floor(value + 0.5)


def expected_rect(game, panel, size):
    width, height = size
    return (game["x"] + lround(panel["anchorX"] * game["width"]) - width // 2,
            game["y"] + lround(panel["anchorY"] * game["height"]) - height // 2)


class OverlayWalk:
    def overlays(self: Any):
        return self.request("overlays")

    def fixture_panel(self: Any):
        data = self.profile(PROFILE)[0]["json"] or {}
        panel = next((p for p in data.get("panels", []) if p.get("id") == PANEL), None)
        if panel is None:
            raise AssertionError("fixture profile has no main panel")
        return {"anchorX": panel.get("anchorX", 0.5), "anchorY": panel.get("anchorY", 0.5)}

    def fixture_session(self: Any):
        daemon = self.state()["daemon"]
        require(daemon["reachable"], "LayerD is not reachable")
        payload = daemon["status"].get("payload", daemon["status"])
        sessions = [s for s in payload.get("sessions", []) if s.get("profileId") == "fixture"]
        require(len(sessions) <= 1, f"more than one fixture session: {sessions}")
        return sessions[0] if sessions else None

    def await_overlays(self: Any, count, label, seconds=6.0):
        """Poll until exactly `count` overlay windows are mapped, then capture."""
        deadline = time.monotonic() + seconds
        while True:
            found = self.overlays()
            if len(found["mapped"]) == count:
                self.capture(label)
                return found
            require(time.monotonic() < deadline,
                    f"{label}: expected {count} mapped overlay window(s), saw {found}")
            time.sleep(0.2)

    def await_anchored(self: Any, game, label, seconds=6.0):
        """The single mapped overlay at the exact anchored rectangle on `game`."""
        panel = self.fixture_panel()
        deadline = time.monotonic() + seconds
        while True:
            mapped = self.overlays()["mapped"]
            if len(mapped) == 1:
                window = mapped[0]
                x, y = expected_rect(game, panel, (window["width"], window["height"]))
                if abs(window["x"] - x) <= SLACK and abs(window["y"] - y) <= SLACK:
                    self.capture(label)
                    self.require_painted(window, label)
                    return window
            require(time.monotonic() < deadline,
                    f"{label}: overlay not anchored at {panel} on {game}: {mapped}")
            time.sleep(0.2)

    def require_painted(self: Any, window, label):
        width, height = self.shot["screen"]
        left, top = max(0, window["x"]), max(0, window["y"])
        right = min(width, window["x"] + window["width"])
        bottom = min(height, window["y"] + window["height"])
        require(right > left and bottom > top, f"{label}: overlay {window} is off screen")
        inside = self.request("inspect", screenshot=self.shot["screenshot"], rgb=[80, 186, 200],
                              region=[left, top, right - left, bottom - top])
        require(inside["count"] > 100,
                f"{label}: overlay window is not painted ({inside['count']} panel pixels)")

    def pixel(self: Any, x, y):
        runs = self.request("inspect", screenshot=self.shot["screenshot"],
                            region=[x, y, 1, 1], runs=True)["runs"]
        return runs[0][2]

    def inside(self: Any, window, game):
        """The panel's anchor point (its centre) lies on the target. The
        centred-anchor rule does not clamp, so a wide panel near an edge may
        overhang the client area, exactly as on Windows."""
        x, y = window["x"] + window["width"] // 2, window["y"] + window["height"] // 2
        return (game["x"] <= x < game["x"] + game["width"] and
                game["y"] <= y < game["y"] + game["height"])

    def open_saved_profile(self: Any, filename, label):
        """Select by both saved name and ID; discard a preceding failed draft."""
        data = self.store()[filename]["json"]
        for index in self.profile_rows(data["name"]):
            self.row(index, label + f"-row-{index}")
            if self.dialog():
                self.press("Discard", label + "-discard-failed-draft")
            if self.text("Profile ID (read-only)") == data["id"]:
                return
        raise AssertionError(f"saved profile {filename!r} did not open")

    def isolate_overlay_fixture(self: Any):
        """Previous cases intentionally leave imported copies and a failed draft.
        Disable other saved profiles through real UI edits, never store writes;
        this is the owned disposable fixture, not a user's profile directory.
        """
        self.focus(EDITOR, "overlay-isolation-editor")
        self.key("ctrl+1", "overlay-isolation-profile-page")
        for filename, entry in sorted(self.store().items()):
            data = entry.get("json")
            if (not filename.endswith(".json") or filename == PROFILE or
                    not isinstance(data, dict) or not data.get("enabled", True)):
                continue
            self.open_saved_profile(filename, "overlay-isolation-" + data["id"])
            self.click(self.find("check box", "Profile enabled"), "overlay-isolation-disable")
            self.save("overlay-isolation-saved")
            require(self.store()[filename]["json"].get("enabled") is False,
                    f"another profile stayed enabled: {filename}")
        self.open_saved_profile(PROFILE, "overlay-isolation-fixture")
        # Without another enabled profile, selecting the current row does not
        # reload a failed name draft. Restore that name through the UI too.
        saved = self.profile(PROFILE)[0]["json"]
        if self.text("Profile name", "entry") != saved["name"]:
            self.type_into("entry", "Profile name", saved["name"], "overlay-isolation-restore-name")
            self.save("overlay-isolation-name-saved")
        if not saved.get("enabled", True):
            self.click(self.find("check box", "Profile enabled"), "overlay-isolation-enable-fixture")
            self.save("overlay-isolation-fixture-saved")
        require([name for name, row in self.store().items()
                 if name.endswith(".json") and isinstance(row.get("json"), dict) and
                 row["json"].get("enabled", True)] == [PROFILE],
                "overlay fixture was not isolated")

    def case_overlay_focus_changes(self: Any):
        # A preceding fixture operation leaves no fresh screenshot to act on.
        self.capture("overlay-focus-start")
        self.isolate_overlay_fixture()
        self.focus(GAME, "overlay-focus-game")
        self.key("e", "overlay-focus-binding")
        game = self.window(GAME)
        shown = self.await_anchored(game, "overlay-focus-shown")
        require(self.inside(shown, game), f"overlay {shown} is not inside the target {game}")
        session = self.fixture_session()
        require(session is not None and session["foreground"] and session["overlayWindows"] == 1 and
                int(session["window"]) == game["id"],
                f"LayerD does not report one foreground overlay on the target: {session}")
        self.focus(EDITOR, "overlay-focus-editor")
        self.await_overlays(0, "overlay-focus-hidden")
        away = self.fixture_session()
        require(away is not None and not away["foreground"] and away["overlayWindows"] == 0,
                f"focus loss did not keep the session without overlays: {away}")
        self.focus(GAME, "overlay-focus-regained")
        back = self.await_anchored(self.window(GAME), "overlay-focus-restored")
        require((back["x"], back["y"], back["width"], back["height"]) ==
                (shown["x"], shown["y"], shown["width"], shown["height"]),
                f"overlay moved across focus changes: {shown} -> {back}")
        return (f"overlay {shown['width']}x{shown['height']}+{shown['x']}+{shown['y']}; "
                "hidden while LayerE was focused (session kept); restored in place")

    def case_overlay_target_resize(self: Any):
        self.capture("resize-start")
        self.focus(GAME, "resize-game-focused")
        start = self.window(GAME)
        before = self.await_anchored(start, "resize-anchored-before")
        x, y = self.corner(start)
        self.act("drag", "resize-target-smaller", x=x, y=y, to_x=x - 400, to_y=y - 250)
        small = self.window(GAME)
        require(small["width"] <= start["width"] - 300 and small["height"] <= start["height"] - 200,
                f"corner drag did not shrink the target: {start} -> {small}")
        shrunk = self.await_anchored(small, "resize-anchored-smaller")
        require(self.inside(shrunk, small), f"overlay {shrunk} left the resized target {small}")
        require((shrunk["x"], shrunk["y"]) != (before["x"], before["y"]),
                f"overlay did not move with the target: {before} -> {shrunk}")
        x, y = self.corner(small)
        self.act("drag", "resize-target-restored", x=x, y=y,
                 to_x=x + start["width"] - small["width"], to_y=y + start["height"] - small["height"])
        restored = self.window(GAME)
        require(abs(restored["width"] - start["width"]) <= SLACK and
                abs(restored["height"] - start["height"]) <= SLACK,
                f"second corner drag did not restore the target: {restored}")
        again = self.await_anchored(restored, "resize-anchored-restored")
        return (f"target {start['width']}x{start['height']} -> {small['width']}x{small['height']} -> "
                f"{restored['width']}x{restored['height']}; overlay +{before['x']}+{before['y']} -> "
                f"+{shrunk['x']}+{shrunk['y']} -> +{again['x']}+{again['y']}")

    def click_target(self: Any, game, x, y, label):
        result = self.request("click_through", screenshot=self.shot["screenshot"],
                              window=game["id"], x=x, y=y)
        self.capture(label)
        presses = [e for e in result["events"] if e.get("type") == "ButtonPress"]
        releases = [e for e in result["events"] if e.get("type") == "ButtonRelease"]
        require(len(presses) == 1 and len(releases) == 1,
                f"{label}: the target did not receive exactly one click: {result}")
        press = presses[0]
        require(not press["synthetic"] and press.get("button") == 1 and
                (press.get("rootX"), press.get("rootY")) == (x, y) and
                (press.get("x"), press.get("y")) == (x - game["x"], y - game["y"]),
                f"{label}: unexpected delivery {press}")
        return press

    def case_overlay_click_through(self: Any):
        self.capture("click-start")
        self.focus(GAME, "click-game-focused")
        game = self.window(GAME)
        overlay = self.await_anchored(game, "click-overlay-shown")
        # Positive control: the observer sees a click on the target itself.
        control = self.click_target(game, game["x"] + 40, game["y"] + 40, "click-target-control")
        overlay = self.await_anchored(self.window(GAME), "click-overlay-before")
        x, y = overlay["x"] + overlay["width"] // 2, overlay["y"] + overlay["height"] // 2
        colour = self.pixel(x, y)
        require(colour in PANEL_COLORS, f"click point {x},{y} is not an overlay pixel ({colour})")
        through = self.click_target(game, x, y, "click-through-overlay")
        after = self.await_anchored(self.window(GAME), "click-overlay-after")
        require(after["id"] == overlay["id"], "the overlay window was replaced by the click")
        return (f"control click at window ({control['x']},{control['y']}); click on overlay pixel "
                f"#{colour} at root ({x},{y}) delivered to the target at ({through['x']},{through['y']})")

    def set_enabled(self: Any, enabled, label):
        self.focus(EDITOR, label + "-focused")
        self.key("ctrl+1", label + "-profile-page")
        box = self.find("check box", "Profile enabled")
        if bool(box.get("checked")) != enabled:
            self.click(box, label + "-toggled")
        self.until(lambda _: (self.profile(PROFILE)[0]["json"] or {}).get("enabled") is enabled,
                   f"{label}: enabled={enabled} was not saved", seconds=8)

    def case_overlay_cleanup(self: Any):
        self.capture("cleanup-start")
        self.set_enabled(False, "cleanup-disable")
        self.focus(GAME, "cleanup-game-focused")
        deadline = time.monotonic() + 6.0
        while (session := self.fixture_session()) is not None:
            require(time.monotonic() < deadline, f"disabled profile kept its session: {session}")
            time.sleep(0.2)
        empty = self.await_overlays(0, "cleanup-disabled-no-overlay")
        require(not empty["unmapped"], f"disabled profile left overlay windows: {empty}")
        self.set_enabled(True, "cleanup-enable")
        self.focus(GAME, "cleanup-game-refocused")
        restored = self.await_anchored(self.window(GAME), "cleanup-overlay-restored")
        client = restored["client"]
        self.request("daemon_stop")
        stopped = self.await_overlays(0, "cleanup-daemon-stopped")
        leftover = [w for w in stopped["mapped"] + stopped["unmapped"] if w["client"] == client]
        require(not leftover, f"stopped LayerD left overlay windows: {leftover}")
        self.request("daemon_start")
        self.capture("cleanup-daemon-started")  # The restart is a fixture operation.
        self.focus(GAME, "cleanup-daemon-restarted")
        again = self.await_anchored(self.window(GAME), "cleanup-overlay-after-restart")
        return (f"disabled: no session and no overlay window; re-enabled overlay "
                f"+{restored['x']}+{restored['y']}; LayerD stop left none; restart restored "
                f"+{again['x']}+{again['y']}")
