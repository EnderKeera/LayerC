"""File and Tools menu cases: Options, Save As, profiles folder, Split grouped
bars and editor Appearance. Real X input only."""
from typing import Any

from automation_walkthrough import require

STATUS = "Save / reload outcome"
APPEARANCE = "Appearance — LayerE"
WINDOW_FILL = [0x1F, 0x1F, 0x1F]  # The presets' shared neutral window colour.
# Selected rows: about a quarter of the preset accent over the window colour.
COPPER_SELECTION = [0x4A, 0x39, 0x29]
TEAL_SELECTION = [0x2C, 0x42, 0x3F]
TASKS = "Detected Tasks — LayerE"
GAME = "LayerC Automation Fixture"
CYAN = [80, 186, 200]  # The fixture profile's bar fill, as walkthrough.py samples it.
EXPECTED = {
    "tools-options-save-as": "Options names the X11 capture-exclusion and isolation limits; File > "
                             "Open profiles folder reports the store path; Save As writes one "
                             "renamed copy with the same cooldowns, which Delete then removes.",
    "split-grouped-bars": "A bar shared by two cooldowns splits into two single-cooldown bars "
                          "stacked around the shared anchor, autosaves, and undoes exactly; "
                          "a profile without shared bars is reported unchanged.",
    "editor-appearance": "Without a saved appearance the editor paints Graphite / Copper; Tools > "
                         "Appearance previews a preset live, Apply persists it to "
                         "editor-appearance.json and repaints the editor, Cancel restores the "
                         "applied look, and System restores FLTK's colours.",
    "detected-tasks-placement": "Ctrl+T lists the game (never LayerE) with its executable and PID; "
                                "Use selected task with the exact path autosaves the matcher; "
                                "Position on window maps the preview to the game window, a drag "
                                "moves LayerD's overlay on the game, and both edits undo exactly.",
}


class ToolsWalk:
    def status_text(self: Any, nodes=None):
        return self.find("text", STATUS, nodes).get("text", "")

    def menu_item(self: Any, menu, downs, label):
        self.key(menu, label + "-menu")
        for step in range(downs):
            self.key("Down", f"{label}-down-{step}")
        self.key("Return", label)

    def window_pixels(self: Any, label, *colours):
        self.capture(label)
        editor = self.window("LayerE")
        width, height = self.shot["screen"]
        region = [max(0, editor["x"]), max(0, editor["y"]),
                  min(editor["width"], width - editor["x"]), min(editor["height"], height - editor["y"])]
        return [self.request("inspect", screenshot=self.shot["screenshot"], rgb=rgb,
                             region=region)["count"] for rgb in colours]

    def window_fill_pixels(self: Any, label):
        return self.window_pixels(label, WINDOW_FILL)[0]

    def cyan_near(self: Any, game, center_x, half_width, shot=None):
        # Overlay pixels in a column band of the game window. Other profiles that
        # match the fixture (an earlier case imports a copy) draw their own bars,
        # so a whole-window mean cannot isolate the moved panel.
        shot = shot or self.shot
        width, height = shot["screen"]
        left = max(0, game["x"], int(center_x - half_width))
        right = min(width, game["x"] + game["width"], int(center_x + half_width))
        region = [left, max(0, game["y"]), max(1, right - left), min(game["height"], height - game["y"])]
        return self.request("inspect", screenshot=shot["screenshot"], rgb=CYAN, region=region)

    def window_open(self: Any, prefix, nodes):
        return any(node["depth"] == 1 and node["name"].startswith(prefix) for node in nodes)

    def open_appearance(self: Any, label):
        # Tools: Options, Appearance.
        self.menu_item("alt+t", 2, label)
        self.until(lambda n: self.window_open(APPEARANCE, n), f"Appearance did not open ({label})")
        self.capture(label + "-shown")

    def wait_closed(self: Any, prefix, label):
        self.until(lambda n: not self.window_open(prefix, n), f"{prefix} stayed open")
        self.capture(label)

    def case_tools_options_save_as(self: Any):
        self.focus("LayerE", "tools-focused")
        self.select_fixture("tools-fixture")
        self.key("ctrl+comma", "options-opened")
        options = self.until(lambda n: (t := self.find("text", "Options — LayerE", n).get("text", ""))
                             and t, "Ctrl+, did not open Options")
        require("Unavailable on Linux X11" in options and "Isolated visual recognition" in options,
                f"Options does not name the Linux limits: {options!r}"[:400])
        self.press("Close", "options-closed")
        # File: New, Import, Export JSON, Export pack, Save, Save As, Open profiles folder.
        self.menu_item("alt+f", 7, "folder-opened")
        folder = self.until(lambda n: (t := self.status_text(n)) and "profiles folder" in t
                            and "/layerc/profiles" in t and t,
                            "Open profiles folder showed no outcome with the path")
        before = self.store()
        original = self.profile()[0]["json"]
        self.key("ctrl+shift+s", "save-as-opened")
        self.until(lambda _: self.dialog(), "Save As did not ask for a name")
        self.key("ctrl+a", "save-as-select")
        self.act("type", "save-as-typed", text="Saved as copy")
        self.key("Return", "save-as-accepted")
        self.until(lambda _: len(set(self.store()) - set(before)) == 1,
                   "Save As did not write one new profile")
        added = (set(self.store()) - set(before)).pop()
        copy = self.store()[added]["json"]
        require(copy["name"] == "Saved as copy" and copy["id"] != original["id"] and
                copy["cooldowns"] == original["cooldowns"] and copy["panels"] == original["panels"],
                f"Save As did not write a renamed copy: {copy!r}"[:400])
        require(self.profile()[0]["json"] == original, "Save As changed the original profile")
        # Profile: Duplicate, Delete -- removes the copy that Save As opened.
        self.menu_item("alt+p", 2, "save-as-delete-prompt")
        self.until(lambda _: self.dialog(), "Delete did not prompt for the copy")
        self.press("Delete", "save-as-deleted")
        self.until(lambda _: added not in self.store(), "Delete kept the Save As copy")
        require(set(self.store()) == set(before), "the store differs after removing the copy")
        self.select_fixture("tools-fixture-again")
        return f"Options shown; {folder.splitlines()[0]!r}; Save As wrote {added}, then deleted"

    def case_split_grouped_bars(self: Any):
        self.focus("LayerE", "split-focused")
        self.select_fixture("split-fixture")
        snapshot = self.profile()[0]["json"]
        self.menu_item("alt+t", 3, "split-none")
        self.until(lambda n: "already independent" in self.status_text(n),
                   "Split on independent bars did not report the unchanged profile")
        require(self.profile()[0]["json"] == snapshot, "an empty split changed the profile")
        self.key("ctrl+2", "split-cooldowns")
        self.menu_item("alt+c", 2, "split-duplicate")
        self.autosaved("split-duplicate-saved",
                       lambda: len(self.profile()[0]["json"]["cooldowns"]) == len(snapshot["cooldowns"]) + 1)
        second = self.profile()[0]["json"]["cooldowns"][-1]["id"]
        self.key("ctrl+4", "split-panels")
        self.automation_choice("Panel cooldown membership", len(snapshot["cooldowns"]), "split-member")
        self.click(self.find("check box", "Include selected cooldown"), "split-member-included")
        self.autosaved("split-grouped-saved",
                       lambda: self.profile()[0]["json"]["panels"][0]["cooldowns"][-1:] == [second])
        # Duplicate also gave the copy its own bar; only the shared bar splits.
        before_split = self.profile()[0]["json"]["panels"]
        grouped = before_split[0]
        self.menu_item("alt+t", 3, "split-applied")
        self.autosaved("split-saved",
                       lambda: len(self.profile()[0]["json"]["panels"]) == len(before_split) + 1)
        saved_panels = self.profile()[0]["json"]["panels"]
        panels = saved_panels[:2]
        members = [p["cooldowns"] for p in panels]
        ids = [p["id"] for p in saved_panels]
        require(saved_panels[2:] == before_split[1:], "Split changed the independent bars")
        require(members == [[c] for c in grouped["cooldowns"]] and len(set(ids)) == len(ids) and
                all(p["style"] == "bar" and p["anchorX"] == grouped["anchorX"] for p in panels) and
                panels[0]["anchorY"] < grouped["anchorY"] < panels[1]["anchorY"] and
                abs((panels[0]["anchorY"] + panels[1]["anchorY"]) / 2 - grouped["anchorY"]) < 1e-6,
                f"the split bars are not stacked single-cooldown bars: {panels!r}"[:500])
        for step, label in enumerate(("split", "membership", "duplicate")):
            self.menu_item("alt+e", 1, f"split-undo-{step}-{label}")
        self.autosaved("split-undone", lambda: self.profile()[0]["json"] == snapshot)
        self.base_hash = self.profile()[0]["sha256"]
        return f"bars {ids[:2]} at anchorY {[round(p['anchorY'], 4) for p in panels]}; undone exactly"

    def case_editor_appearance(self: Any):
        self.focus("LayerE", "appearance-focused")
        require(self.state()["editorAppearance"] is None, "an appearance file exists before the case")
        default, copper = self.window_pixels("appearance-default", WINDOW_FILL, COPPER_SELECTION)
        require(default > 20000 and copper > 0,
                f"no saved appearance did not paint Graphite / Copper ({default} neutral, "
                f"{copper} copper selection pixels)")
        self.open_appearance("appearance-opened")
        self.automation_choice("Theme", 1, "appearance-teal")
        self.click(self.find("button", "Apply"), "appearance-applied")
        self.wait_closed(APPEARANCE, "appearance-apply-closed")
        self.until(lambda n: "Appearance saved: Graphite / Teal." in self.status_text(n),
                   "Apply did not report the saved appearance")
        saved = self.state()["editorAppearance"]
        require(saved == {"appearanceVersion": 1, "theme": "teal"},
                f"editor-appearance.json is unexpected: {saved!r}")
        teal, selection, stale = self.window_pixels("appearance-teal-painted", WINDOW_FILL,
                                                    TEAL_SELECTION, COPPER_SELECTION)
        require(teal > 20000 and selection > 0 and stale == 0,
                f"the editor was not repainted Teal ({teal} neutral; selection pixels "
                f"{copper} copper -> {selection} teal, {stale} copper left)")
        self.open_appearance("appearance-reopened")
        self.automation_choice("Theme", 3, "appearance-system-preview")
        previewed = self.window_fill_pixels("appearance-system-previewed")
        require(previewed < teal // 10, f"System was not previewed live ({previewed} neutral pixels)")
        self.click(self.find("button", "Cancel"), "appearance-cancelled")
        self.wait_closed(APPEARANCE, "appearance-cancel")
        restored = self.window_fill_pixels("appearance-cancel-restored")
        require(restored > teal // 2 and self.state()["editorAppearance"] == saved,
                f"Cancel did not restore the applied look ({restored} pixels)")
        self.open_appearance("appearance-final")
        self.automation_choice("Theme", 3, "appearance-system")
        self.click(self.find("button", "Apply"), "appearance-system-applied")
        self.wait_closed(APPEARANCE, "appearance-system-closed")
        self.until(lambda n: "Appearance saved: System" in self.status_text(n),
                   "Apply did not report the System appearance")
        require(self.state()["editorAppearance"] == {"appearanceVersion": 1, "theme": "system"},
                "System was not persisted")
        final = self.window_fill_pixels("appearance-system-restored")
        require(final < teal // 10, f"System did not restore FLTK's colours ({final} pixels)")
        return f"neutral window pixels {default} (Copper) -> {teal} (Teal) -> {previewed} " \
               f"(preview) -> {restored} (Cancel) -> {final} (System); selection pixels " \
               f"{copper} copper -> {selection} teal"

    def named(self: Any, name_part, nodes):
        return [n for n in nodes if name_part in (n.get("name") or "")]

    def case_detected_tasks_placement(self: Any):
        self.focus("LayerE", "tasks-focused")
        self.select_fixture("tasks-fixture")
        snapshot = self.profile()[0]["json"]
        self.key("ctrl+t", "tasks-opened")
        self.until(lambda n: self.window_open(TASKS, n), "Ctrl+T did not open Detected Tasks")
        nodes = self.nodes()
        tasks = self.find("list", "Detected tasks", nodes)
        start = nodes.index(tasks)
        rows = [n["name"] for n in nodes[start + 1:] if n["depth"] == tasks["depth"] + 1
                and n["role"] == "list item"]
        game_rows = [i for i, row in enumerate(rows) if row.startswith("lctest.exe") and GAME in row]
        require(len(game_rows) == 1 and not any("LayerE" in row for row in rows),
                f"Detected Tasks rows are unexpected: {rows!r}")
        require(rows[game_rows[0]].split("\t")[-1].isdigit(), f"the game row names no PID: {rows!r}")
        x, y, w, _ = tasks["extents"]
        self.act("click", "tasks-first-row", x=x + w // 2, y=y + 10)
        for step in range(game_rows[0]):
            self.key("Down", f"tasks-row-down-{step}")
        self.until(lambda n: any((node.get("name") or "").endswith("/lctest.exe") for node in n),
                   "the selected game row shows no executable path")
        self.click(self.find("check box", "Match the exact path"), "tasks-exact-path")
        self.click(self.find("button", "Use selected task"), "tasks-use")
        self.autosaved("tasks-use-saved", lambda: self.profile()[0]["json"]["game"]
                       .get("executablePath", "").endswith("/lctest.exe"))
        require(self.profile()[0]["json"]["game"]["executableName"] == "lctest.exe",
                "Use selected task changed the executable name")
        self.click(self.find("button", "Position on window"), "tasks-position")
        self.until(lambda n: not self.window_open(TASKS, n), "Position on window kept Detected Tasks open")
        status = self.until(lambda n: (t := self.status_text(n)).startswith(f'Positioning on "{GAME}"') and t,
                            "positioning did not start on the game window")
        game = self.window(GAME)
        self.focus(GAME, "placement-game")
        self.key("e", "placement-overlay-before")
        before = self.overlay_x()
        before_shot = self.shot
        self.focus("LayerE", "placement-editor")
        self.key("ctrl+4", "placement-panels")
        preview = next(n for n in self.nodes() if (n.get("name") or "") == "Local panel preview")
        px, py, pw, ph = preview["extents"]
        scale = min((pw - 10) / game["width"], (ph - 10) / game["height"])
        fw, fh = int(game["width"] * scale), int(game["height"] * scale)
        fx, fy = px + 5 + (pw - 10 - fw) // 2, py + 5 + (ph - 10 - fh) // 2
        panel = self.profile()[0]["json"]["panels"][0]
        sx, sy = fx + round(panel["anchorX"] * fw), fy + round(panel["anchorY"] * fh)
        target = max(0.1, panel["anchorX"] - 0.4)
        self.act("drag", "placement-drag", x=sx, y=sy, to_x=fx + round(target * fw), to_y=sy)
        self.autosaved("placement-saved",
                       lambda: abs(self.profile()[0]["json"]["panels"][0]["anchorX"] - target) < 0.02)
        moved = self.profile()[0]["json"]["panels"][0]
        require(abs(moved["anchorY"] - panel["anchorY"]) < 0.02, f"the drag changed anchorY: {moved!r}")
        self.focus(GAME, "placement-game-after")
        self.key("e", "placement-overlay-after")
        after = self.overlay_x()
        # The overlay centres the bar on the anchor; the band is narrower than the move.
        band = 0.15 * game["width"]
        expected = game["x"] + target * game["width"]
        require(self.cyan_near(game, expected, band, before_shot)["count"] < 100,
                f"a bar was already near x={expected:.0f} before the drag")
        found = self.cyan_near(game, expected, band)
        require(found["count"] > 100 and abs(found["meanX"] - expected) < 0.05 * game["width"],
                f"the overlay did not follow: no bar near x={expected:.0f} ({found!r}); "
                f"window mean {before:.1f} -> {after:.1f}")
        after = found["meanX"]
        self.focus("LayerE", "placement-end-focus")
        self.click(self.find("button", "End positioning"), "placement-ended")
        self.until(lambda n: "Positioning on the window ended." in self.status_text(n),
                   "End positioning reported nothing")
        for step in range(2):
            self.menu_item("alt+e", 1, f"placement-undo-{step}")
        self.autosaved("placement-undone", lambda: self.profile()[0]["json"] == snapshot)
        self.base_hash = self.profile()[0]["sha256"]
        return (f"{len(rows)} rows; exact path set; {status.split(':')[0]}; anchorX "
                f"{panel['anchorX']:.3f} -> {moved['anchorX']:.3f}; overlay {before:.1f} -> {after:.1f}")
