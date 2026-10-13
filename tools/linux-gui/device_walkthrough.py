"""Focused device binding capture: real X key and pointer input only."""
import time
from typing import Any

from automation_walkthrough import require

CAPTURE = "Capture focused binding"
EXPECTED = {
    "device-capture": "Capture focused chord records a physical key and a right click on the "
                      "dialog's mouse area as one chord (Clear restarts it; Escape cancels "
                      "without a write); the dialog states the gamepad and Generic HID limits; "
                      "Accept autosaves the binding and Undo removes it exactly.",
}


class DeviceWalk:
    @staticmethod
    def label_starting(prefix, nodes):
        for node in nodes:
            if node["role"] == "label" and node["name"].startswith(prefix):
                return node["name"]
        return None

    def capture_dialog(self: Any, label):
        self.click(self.find("button", "Capture focused chord..."), label)
        found = self.until(lambda _: (d := self.dialog()) and d[0]["name"] == CAPTURE and d,
                           "the capture dialog did not open")
        return found[1]

    def capture_status(self: Any, prefix, description):
        return self.until(lambda n: self.label_starting(prefix, n), description)

    def case_device_capture(self: Any):
        self.select_fixture("device-fixture")
        self.key("ctrl+3", "device-bindings")
        snapshot = self.profile()[0]["json"]
        nodes = self.capture_dialog("device-capture-open")
        gamepad = self.label_starting("Gamepad:", nodes)
        require(gamepad is not None and self.label_starting("Generic HID capture is unavailable", nodes),
                "the capture dialog does not state its gamepad and HID limits")
        self.key("Escape", "device-capture-cancelled")
        self.until(lambda _: self.dialog() is None, "Escape did not close the capture dialog")
        time.sleep(0.8)  # past the 400 ms autosave pause, so a stray write would show
        require(self.profile()[0]["json"] == snapshot, "a cancelled capture changed the profile")

        self.capture_dialog("device-capture-reopen")
        self.key("q", "device-capture-key")
        self.capture_status("Captured: ", "the focused key press was not captured")
        self.press("Clear", "device-capture-clear")
        self.capture_status("Cleared.", "Clear did not restart the chord")
        self.key("q", "device-capture-key-again")
        pad = self.find("label", "Mouse capture area: click a button or turn the wheel here")
        self.click(pad, "device-capture-right-click", button=3)
        chord = self.capture_status("Captured: ", "the capture status vanished")
        self.until(lambda n: (s := self.label_starting("Captured: ", n)) and " + Mouse button 2" in s,
                   f"the right click was not captured as mouse button 2: {chord!r}")
        self.press("Accept", "device-capture-accepted")
        cooldown = snapshot["cooldowns"][0]["id"]
        expected = snapshot["bindings"] + [{"cooldownId": cooldown, "chord": [
            {"device": "keyboard", "code": 16}, {"device": "mouse", "code": 2}]}]
        self.autosaved("device-capture-saved", lambda: self.profile()[0]["json"]["bindings"] == expected)
        self.key("alt+e", "device-edit-menu")
        self.key("Down", "device-edit-undo")
        self.key("Return", "device-capture-undone")
        self.autosaved("device-undo-saved", lambda: self.profile()[0]["json"] == snapshot)
        self.base_hash = self.profile()[0]["sha256"]
        return f"Q + Mouse button 2 saved and undone; {gamepad}"
