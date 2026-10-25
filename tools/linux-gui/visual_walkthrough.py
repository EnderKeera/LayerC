"""Visual calibration cases: real X input on runner-owned windows only."""
import re
import time
from typing import Any

from automation_walkthrough import require

EXPECTED = {
    "visual-pixel-calibration": "Refuse unacknowledged capture; capture the matching X11 fixture, "
                                "drag a region, sample its color, reject invalid input, cancel "
                                "Exact quality, and durably apply a pixel trigger.",
    "visual-template-calibration": "Capture a structured reference, reject malformed, oversized "
                                   "and duplicate PNGs, paint a mask, reach the four-reference "
                                   "bound, cancel without writes, then apply canonical assets.",
    "visual-shared-preservation": "Cancel, clone and update a shared observation; cancel and "
                                  "confirm removal; preserve the other cooldown and undo/redo.",
    "visual-lifecycle": "Preserve the unavailable isolated runtime, show unavailable capture "
                        "without losing saved settings, and measure a stopped calibration view.",
}
NOTICE = "I acknowledge passive capture of the selected game window"
OUTCOME = "Capture / live match status"


class VisualWalk:
    def visual_module(self: Any, stem="fixture"):
        module = self.state()["modules"].get(f"{stem}/visual.json")
        require(module is not None and isinstance(module.get("json"), dict),
                f"saved {stem} visual sidecar is missing or unreadable")
        return module

    def visual_durable(self: Any):
        state = self.state()
        return ({p["name"]: p.get("sha256") for p in state["profiles"]},
                {name: module["sha256"] for name, module in state["modules"].items()},
                state["assets"])

    def calibrate(self: Any, label):
        self.click(self.find("button", "Calibrate visual..."), label)
        self.until(lambda _: self.dialog(), "calibration modal did not open")

    def visual_status(self: Any):
        return self.text(OUTCOME)

    def visual_import(self: Any, name, label):
        self.click(self.find("button", "Import PNG reference..."), label + "-open")
        self.choose("Import a PNG template reference", self.state()["exchange"] + "/" + name, label)

    def visual_preview_drag(self: Any, label, start, end, mask=False):
        node = self.find("canvas", "Calibration preview")
        x, y, width, height = node["extents"]
        # Use the observed client geometry, not the editor or screen size.
        fixture = self.window("LayerC Automation Fixture")
        require(fixture["width"] > 0 and fixture["height"] > 0,
                "fixture has no nonempty client geometry")
        aspect = 1 if mask else fixture["width"] / fixture["height"]
        iw, ih = min(width, height * aspect), min(height, width / aspect)
        left, top = x + (width - iw) / 2, y + (height - ih) / 2
        self.act("drag", label, x=round(left + iw * start[0]), y=round(top + ih * start[1]),
                 to_x=round(left + iw * end[0]), to_y=round(top + ih * end[1]))

    def visual_prepare_fixture(self: Any, label):
        # Uncomposited Xvfb uses bounded direct capture: keep the controlled
        # client exposed, and restore the 400x300 area the fixture paints.
        fixture = self.window("LayerC Automation Fixture")
        screen_width, screen_height = self.shot["screen"]
        require(screen_width >= 424 and screen_height >= 380,
                "display cannot expose the controlled capture fixture")
        self.act("set_geometry", label + "-fixture-exposed", window=fixture["id"],
                 x=screen_width - 424, y=40, width=400, height=300)
        fixture = self.window("LayerC Automation Fixture")
        require(fixture["width"] == 400 and fixture["height"] == 300,
                "window manager did not restore the controlled capture area")
        modal = self.dialog()
        require(modal is not None, "capture requires the calibration dialog")
        mx, my, mw, mh = modal[0]["extents"]
        fx, fy, fw, fh = (fixture[k] for k in ("x", "y", "width", "height"))
        require(fx >= 0 and fx + fw <= screen_width and
                fy >= 0 and fy + fh <= screen_height and
                (fx >= mx + mw or fx + fw <= mx or fy >= my + mh or fy + fh <= my),
                "direct capture fixture is clipped or obscured by calibration")

    def visual_start(self: Any, label):
        self.visual_prepare_fixture(label)
        self.click(self.find("button", "Rescan windows"), label + "-rescan")
        _, items = self.choice_items("Calibration target window")
        require(len(items) == 1 and items[0]["name"].startswith("LayerC Automation Fixture"),
                f"window picker did not bind to the controlled fixture: {items!r}")
        self.click(self.find("check box", NOTICE), label + "-consented")
        self.click(self.find("check box", "Live preview"), label + "-live")
        status = self.until(lambda n: (t := self.find("text", OUTCOME, n).get("text", ""))
                            and re.match(r"(?:Match|No match):", t) and t,
                            "X11 capture did not produce an available frame")
        self.click(self.find("check box", "Live preview"), label + "-frozen")
        require(self.visual_status().startswith("Stopped. Capture session released"),
                "stopping preview did not release the capture session")
        return status

    def visual_apply(self: Any, label):
        self.click(self.find("button", "Apply calibration"), label)
        self.until(lambda _: self.dialog() is None, "calibration did not close after Apply")
        outcome = self.text("Save / reload outcome")
        require("acknowledged" in outcome, f"calibration was not durably applied/reloaded: {outcome!r}")
        return outcome

    def case_visual_pixel_calibration(self: Any):
        self.select_fixture("visual-fixture")
        self.key("ctrl+6", "visual-page")
        self.calibrate("pixel-calibration-open")
        before = self.visual_durable()
        self.click(self.find("check box", "Live preview"), "pixel-unacknowledged")
        require("acknowledge" in self.visual_status() and
                not self.find("check box", "Live preview").get("checked"),
                "capture started without the notice acknowledgement")
        captured = self.visual_start("pixel")
        # The rectangle is wholly within the fixture\'s 100x100 black corner.
        expected_region = {"x": 0.02, "y": 0.02, "width": 0.18, "height": 0.23}
        self.visual_preview_drag("pixel-region-drag", (0.02, 0.02), (0.20, 0.25))
        labels = {"x": "Region X (0–1)", "y": "Region Y (0–1)",
                  "width": "Region width", "height": "Region height"}
        region = {key: float(self.text(labels[key], "entry")) for key in expected_region}
        require(all(abs(region[key] - value) <= 0.006 for key, value in expected_region.items()),
                f"preview drag did not select the controlled rectangle: {region!r}")
        self.type_into("entry", "Target color #RRGGBB", "#123456", "pixel-before-sampling")
        self.click(self.find("button", "Sample selection color"), "pixel-color-sampled")
        sampled = self.text("Target color #RRGGBB", "entry")
        require(sampled == "#000000", f"sampling did not read the fixture black corner: {sampled!r}")
        self.type_into("entry", "Stable samples (1–15)", "nan", "pixel-invalid-samples")
        self.click(self.find("button", "Apply calibration"), "pixel-invalid-refused")
        require(self.dialog() is not None and self.visual_durable() == before,
                "invalid calibration closed or wrote durable state")
        self.type_into("entry", "Stable samples (1–15)", "2", "pixel-valid-samples")
        # Exact quality is a separate consent, not implicit with capture.
        self.automation_choice("Capture quality", 2, "pixel-quality-exact")
        self.press("Cancel", "pixel-quality-cancelled")
        _, quality = self.choice_items("Capture quality")
        require(quality[1].get("selected"), "cancelled Exact quality was retained")
        self.automation_choice("Trigger mode", 1, "pixel-confirm-input")
        self.type_into("entry", "Input timeout (50–5000 ms)", "750", "pixel-timeout")
        self.automation_choice("Capture unavailable", 1, "pixel-fallback")
        outcome = self.visual_apply("pixel-applied")
        visual = self.visual_module()["json"]
        require(len(visual["observations"]) == 1 and visual["observations"][0]["type"] == "pixel"
                and visual["observations"][0]["stableSamples"] == 2
                and visual["triggers"][0]["mode"] == "confirmInput"
                and visual["triggers"][0]["confirmationTimeoutMs"] == 750
                and visual["triggers"][0]["onUnavailable"] == "triggerFromInput",
                f"saved pixel calibration lost controls: {visual!r}")
        saved = visual["observations"][0]
        require(all(abs(saved["region"][key] - region[key]) <= 0.00001 for key in region)
                and saved["predicate"]["target"] == sampled,
                f"durable pixel observation lost the dragged rectangle or sampled color: {saved!r}")
        self.base_hash = self.profile()[0]["sha256"]
        return f"acknowledged X11 capture and drag; {captured}; {outcome}"

    def case_visual_template_calibration(self: Any):
        before = self.visual_durable()
        self.calibrate("template-cancel-open")
        self.visual_import("reference-0.png", "template-cancel-import")
        self.click(self.find("button", "Cancel calibration"), "template-cancelled")
        require(self.visual_durable() == before, "cancel published a template/module/base profile")
        self.calibrate("template-open")
        self.visual_start("template")
        # Do not inherit the preceding pixel calibration's nearly uniform crop.
        # The controlled fixture has a black corner on a white client area.
        self.visual_preview_drag("template-capture-region", (0.02, 0.02), (0.95, 0.95))
        self.click(self.find("button", "Capture reference"), "template-reference-captured")
        require(self.text("Template references").startswith("1/4"),
                f"capture did not create a structured reference: {self.visual_status()!r}")
        # Test malformed PNG and the explicit decoder dimension bound.
        for name in ("invalid-reference.png", "oversize-reference.png"):
            self.visual_import(name, "template-" + name)
            require(self.text("Template references").startswith("1/4"),
                    "invalid PNG added a reference")
        self.click(self.find("button", "Clear references"), "template-clear")
        self.visual_import("reference-0.png", "template-reference-0")
        self.visual_import("reference-0.png", "template-duplicate")
        require(self.text("Template references").startswith("1/4")
                and "duplicate" in self.visual_status().lower(), "duplicate reference was accepted")
        self.automation_choice("Preview tool", 1, "template-mask-tool")
        # Every 32x32 pixel is erased: Apply must refuse, but Restore remains usable.
        for row in range(1, 32, 3):
            y = (row + 0.5) / 32
            self.visual_preview_drag(f"template-mask-erase-{row}", (0.5 / 32, y), (31.5 / 32, y), mask=True)
        self.click(self.find("button", "Apply calibration"), "template-empty-mask-refused")
        require(self.dialog() is not None and "visible pixels" in self.visual_status()
                and self.visual_durable() == before, "an empty mask was accepted or published")
        self.automation_choice("Preview tool", 2, "template-mask-restore-tool")
        for row in range(1, 32, 3):
            y = (row + 0.5) / 32
            self.visual_preview_drag(f"template-mask-restore-{row}", (0.5 / 32, y), (31.5 / 32, y), mask=True)
        self.automation_choice("Preview tool", 1, "template-mask-repaired")
        self.visual_preview_drag("template-mask-painted", (0.2, 0.2), (0.45, 0.45), mask=True)
        self.automation_choice("Preview tool", 0, "template-mask-finalized")
        for number in range(1, 4):
            self.visual_import(f"reference-{number}.png", f"template-reference-{number}")
        self.visual_import("reference-4.png", "template-reference-limit")
        require(self.text("Template references").startswith("4/4")
                and "maximum" in self.visual_status().lower(), "four-reference bound was not enforced")
        require(self.visual_durable() == before, "template edits published before Apply")
        self.type_into("entry", "Template confidence (%)", "85", "template-confidence")
        self.type_into("entry", "Scale tolerance (0–20%)", "5", "template-scale")
        outcome = self.visual_apply("template-applied")
        observation = self.visual_module()["json"]["observations"][0]
        refs = observation.get("references", [])
        require(observation["type"] == "template" and len(refs) == 4
                and abs(observation["confidence"] - 0.85) < 1e-6
                and observation["scaleTolerancePercent"] == 5,
                f"saved template calibration lost settings: {observation!r}")
        state = self.state()
        assets = state["assets"]
        require(all(assets.get("fixture/" + ref["asset"] + ".png") == ref["asset"] for ref in refs),
                "canonical asset hashes do not match all saved references")
        alpha = state["assetAlpha"].get("fixture/" + refs[0]["asset"] + ".png", {})
        require(alpha.get("transparentPixels", 0) > 0 and alpha.get("opaquePixels", 0) > 0,
                f"the saved reference lost its painted alpha mask: {alpha!r}")
        self.notes["visual-template-alpha"] = alpha
        self.notes["visual-template-assets"] = refs
        self.base_hash = self.profile()[0]["sha256"]
        return f"real reference capture, PNG bounds, alpha-mask paint, four canonical assets; {outcome}"

    def case_visual_shared_preservation(self: Any):
        self.row(self.row_of("Shared calibration fixture"), "shared-profile")
        self.key("ctrl+6", "shared-visual-page")
        before = self.visual_durable()
        original_triggers = {t["cooldownId"]: t for t in self.visual_module("z-shared")["json"]["triggers"]}
        self.calibrate("shared-cancel-open")
        # Apply needs a captured frame evaluated against the current predicate.
        self.visual_start("shared")
        self.type_into("entry", "Brightness threshold", "70", "shared-threshold")
        self.automation_choice("Calibration predicate", 1, "shared-brightness")
        self.click(self.find("button", "Apply calibration"), "shared-choice-open")
        self.press("Cancel", "shared-choice-cancelled")
        require(self.visual_durable() == before, "cancelled shared choice changed durable state")
        # A cancelled choice is an Apply refusal: the same dialog keeps its draft.
        require(self.dialog() is not None and self.text("Brightness threshold", "entry") == "70",
                "cancelling the shared choice closed calibration or lost its draft")
        self.click(self.find("button", "Apply calibration"), "shared-update-choice")
        self.press("Update shared", "shared-updated")
        updated = self.visual_module("z-shared")["json"]
        require(len(updated["observations"]) == 1
                and updated["observations"][0]["predicate"]["type"] == "brightness"
                and updated["observations"][0]["predicate"]["threshold"] == 70
                and {t["cooldownId"]: t for t in updated["triggers"]} == original_triggers,
                "updating the shared observation lost trigger settings or its new predicate")
        shared_observation = updated["observations"][0]
        self.calibrate("shared-clone-open")
        self.visual_start("shared-clone")
        self.automation_choice("Calibration predicate", 0, "shared-clone-average")
        self.click(self.find("button", "Apply calibration"), "shared-clone-choice")
        self.press("Clone for cooldown", "shared-cloned")
        visual = self.visual_module("z-shared")["json"]
        triggers = {t["cooldownId"]: t["observationId"] for t in visual["triggers"]}
        require(len(visual["observations"]) == 2 and triggers["ability"] != triggers["sibling"]
                and {t["cooldownId"]: t for t in visual["triggers"]} == {
                    "ability": {**original_triggers["ability"], "observationId": triggers["ability"]},
                    "sibling": original_triggers["sibling"]}
                and next(o for o in visual["observations"] if o["id"] == "shared-observation") == shared_observation,
                "clone changed the shared sibling observation or trigger settings")
        self.click(self.find("button", "Remove visual assignment"), "shared-remove-cancel")
        self.press("Cancel", "shared-remove-cancelled")
        self.click(self.find("button", "Remove visual assignment"), "shared-remove-open")
        self.press("Remove assignment", "shared-removed")
        self.key("ctrl+z", "shared-remove-undo")
        require("No visual trigger" not in self.text("Visual assignment"), "remove was not undoable")
        self.key("ctrl+y", "shared-remove-redo")
        self.save("shared-remove-saved")
        remaining = self.visual_module("z-shared")["json"]
        require(remaining["observations"] == [shared_observation]
                and remaining["triggers"] == [original_triggers["sibling"]],
                "remove discarded or changed the sibling's observation or trigger settings")
        self.select_fixture("visual-fixture-return")
        return "shared choice cancel, update and clone; removal cancel, undo/redo and sibling preservation"

    def case_visual_lifecycle(self: Any):
        self.key("ctrl+6", "visual-lifecycle-page")
        self.click(self.find("check box", "Isolated visual runtime"), "visual-isolated")
        require("Windows-only" in self.text("Visual availability / preservation"),
                "isolated runtime did not retain its unavailable reason")
        self.save("visual-isolated-saved")
        require(self.profile()[0]["json"].get("visualRuntime") == "isolated", "isolated choice was lost")
        self.click(self.find("check box", "Isolated visual runtime"), "visual-inline")
        self.save("visual-inline-saved")
        before = self.visual_durable()
        self.calibrate("visual-stopped-open")
        self.click(self.find("button", "Rescan windows"), "visual-stopped-rescan")
        self.click(self.find("check box", "Live preview"), "visual-unavailable")
        require(self.visual_status().startswith("Unavailable:"), "capture refusal lacked its reason")
        self.automation_resources("calibration-stopped")
        status = self.visual_status()
        time.sleep(1.0)
        require(self.visual_status() == status, "refused capture retained a polling timer")
        self.click(self.find("button", "Cancel calibration"), "visual-stopped-close")
        require(self.visual_durable() == before, "lifecycle checks changed durable visual data")
        self.base_hash = self.profile()[0]["sha256"]
        return "unavailable isolated setting preserved, capture refusal visible; stopped view idle sample"
