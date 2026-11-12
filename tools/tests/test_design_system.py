"""Planted violations for every mechanical design-system rule."""
from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from typing import TypedDict

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_design_system as check
import design_system_checks as rules


class TokenEntry(TypedDict):
    name: str
    value: str | dict[str, str]


class TokenSection(TypedDict):
    tokens: list[TokenEntry]


class SourceRules(unittest.TestCase):
    def visual(self, text, path="src/editor/dialog.cpp"):
        return rules.visual_violations(text, path)

    def test_colors_reject_literals_and_toolkit_colors(self):
        for text in ("auto fill = 0x123456;", "RGB(1, 2, 3);", "RGB(0xff, 0x00, 0x00);", "fl_rgb_color(1, 2, 3);", "fl_color(FL_RED);", "button->color(0xFFFFFF);"):
            with self.subTest(text=text):
                self.assertIn("token-color", [item[1] for item in self.visual(text)])

    def test_colors_accept_token_sources_and_dynamic_colors(self):
        self.assertEqual([], self.visual("auto fill = 0x123456;", rules.NATIVE_THEME))
        self.assertEqual([], self.visual("auto fill = 0x123456;", rules.LINUX_THEME))
        self.assertEqual([], self.visual("RGB(r, g, b); fl_color(role.fill);"))
        self.assertEqual([], self.visual("return value & 0xFFFFFFU;"))
        self.assertEqual([], self.visual("core::AverageColorPredicate{0xFFFFFF, 16};"))
        self.assertEqual([], self.visual("ImageList_AddMasked(images, transparent, RGB(0, 0, 0));"))

    def test_comments_strings_raw_strings_are_not_paint(self):
        text = '// RGB(1,2,3);\n/* RoundRect(); */\nauto s = R"tag(RoundRect(); 0x123456)tag";'
        self.assertEqual([], self.visual(text))
        self.assertEqual([(3, "RoundRect(); 0x123456")], list(check.string_literals(text)))

    def test_diagnostic_line_numbers_survive_multiline_comments(self):
        found = self.visual("/*\ncomment\n*/\nRoundRect();")
        self.assertEqual(4, found[0][0])

    def test_square_chrome_rejects_each_rounding_family(self):
        for name in ("RoundRect", "CreateRoundRectRgn", "FL_ROUNDED_BOX", "FL_RFLAT_BOX", "FL_ROUND_UP_BOX", "FL_OVAL_BOX", "FL_ROUNDED_FRAME"):
            with self.subTest(name=name):
                self.assertIn("square-chrome", [item[1] for item in self.visual(name + ";")])

    def test_only_actual_overlay_preview_can_round(self):
        self.assertEqual([], self.visual("RoundRect();", "src/editor/linux/editor_preview.cpp"))
        text = "void draw_bar_preview() { RoundRect(); }\nvoid draw_dialog() { RoundRect(); }"
        found = self.visual(text, "src/editor/editor_draw.cpp")
        self.assertEqual(1, len(found))
        self.assertEqual(2, found[0][0])
        self.assertEqual(2, len(self.visual(text)))
        self.assertTrue(self.visual("auto fill = 0x123456;", "src/editor/linux/editor_preview.cpp"))

    def test_control_heights_reject_literals_in_single_line_controls(self):
        for widget in ("Fl_Input", "Fl_Choice", "Fl_Button", "EditorChoice", "EditorField<Fl_Input>"):
            with self.subTest(widget=widget):
                self.assertIn("control-height", [item[1] for item in self.visual(f"new {widget}(x, y, width, 28, label);")])
        for text in ('CreateWindowExW(0, L"EDIT", L"", style, x, y, w, 26, parent, 0, 0, 0);',
                     'CreateWindowW(L"BUTTON", L"", style, x, y, w, 26, parent, 0, 0, 0);',
                     "const int button_h = 26;"):
            self.assertIn("control-height", [item[1] for item in self.visual(text)])

    def test_control_heights_allow_metrics_multiline_and_windows(self):
        text = "new Fl_Input(x, y, std::max(10, width), editor_input_height); new Fl_Multiline_Output(x, y, w, 120); new Fl_Double_Window(800, 600);"
        self.assertEqual([], self.visual(text))
        self.assertEqual([], self.visual("const int button_h = 30;", "src/editor/linux/editor_style.hpp"))

    def test_writing_resources_and_both_menu_formats(self):
        path = Path("menu.rc")
        self.assertIn("sentence-case", [v.rule for v in check.check_rc(path, 'MENUITEM "Save Profile", 1')])
        self.assertEqual([], check.check_rc(path, 'MENUITEM "Export JSON...\\tCtrl+E", 1'))
        for text in ('auto item = "&File/Save Profile";', 'AppendMenuW(menu, 0, id, L"Save Profile");'):
            self.assertIn("sentence-case", [v.rule for v in check.check_source(Path("menu.cpp"), text)])
        self.assertEqual([], check.check_source(Path("menu.cpp"), 'auto item = "&File/Save profile";'))

    def test_each_writing_rule_fails_on_a_planted_violation(self):
        for text, rule in (("Open…", "ellipsis"), ("Couldn't save.", "could-not"),
                           ("Done!", "exclamation"), ("Ready 😀", "emoji"),
                           ("Daemon status", "layerd-name"), ("Delay 10ms", "unit-space")):
            with self.subTest(rule=rule):
                self.assertIn(rule, [v.rule for v in check.check_text(Path("menu.rc"), 1, text)])
        self.assertEqual([], check.check_text(Path("x"), 1, "Ready — connecting to LayerD. Delay 10 ms."))
        self.assertEqual([], check.sentence_case_errors("Export JSON to Windows"))
        self.assertEqual([], check.check_source(Path("x.cpp"), '#include "layerc/daemon/client.hpp"'))


class TokenRules(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        fixed = {"background": "#1f1f1f", "panel": "#2d2d2d", "input": "#171717", "border": "#6e6e6e", "divider": "#333333",
                     "text": "#f3f3f3", "secondary": "#b8b8b8", "subtext": "#a3a3a3", "verbose": "#9a9a9a", "disabled": "#808080",
                     "button_hover": "#383838", "row_hover": "#2a2a2a", "pressed": "#262626", "yes": "#00e676", "no": "#ff5a5a",
                     "warning": "#ffd740", "on_status": "#141414"}
        self.fixed = fixed
        colors: dict[str, str | dict[str, str]] = dict(fixed)
        preset_values = dict(zip(("copper", "teal", "plum", "slate"), ("#d88e4b", "#5ab6aa", "#b382e2", "#8fa3b8"), strict=True))
        for preset, value in preset_values.items():
            colors[f"preset-{preset}"] = value
        colors.update({
            "accent": {p: "{preset-" + p + "}" for p in ("copper", "teal", "plum", "slate")},
            "on_accent": dict.fromkeys(("copper", "teal", "plum", "slate"), "#141414"),
            "selection": dict(zip(("copper", "teal", "plum", "slate"), ("#4a3929", "#2c423f", "#41364c", "#393e43"), strict=False)),
            "primary_hover": dict(zip(("copper", "teal", "plum", "slate"), ("#dc9d65", "#72bfb5", "#bd93e4", "#9eafc1"), strict=False)),
            "primary_pressed": dict(zip(("copper", "teal", "plum", "slate"), ("#b07844", "#50968d", "#946eb8", "#788898"), strict=False)),
            "accent_2": dict(zip(("copper", "teal", "plum", "slate"), ("#60b0ff", "#b99bff", "#60b0ff", "#b99bff"), strict=False)),
            "linux-outline-background": "#6e6e6e", "linux-outline-panel": "#777777",
        })
        size = {"win-control-height": "26px", "linux-input-height": "28px", "linux-button-height": "30px"}
        self.data: dict[str, TokenSection] = {section: {"tokens": [{"name": k, "value": v} for k, v in values.items()]}
                     for section, values in (("color", colors), ("size", size), ("spacing", {}), ("radius", {}))}
        self.write_spec()
        fields = "\n".join(f".{name} = 0x{value[1:]}," for name, value in fixed.items())
        presets = "\n".join(f"int {p}_accent = 0x{value[1:]};" for p, value in preset_values.items())
        source = fields + "\n" + presets + "\n"
        self.put(rules.NATIVE_THEME, source)
        self.put(rules.LINUX_THEME, source)
        self.put(rules.COLOR_MATH, "int accent_2_blue = 0x60b0ff; int accent_2_violet = 0xb99bff;\n")
        self.put("src/editor/dialog_layout.hpp", "int native_control_height = 26;")
        self.put("src/editor/linux/editor_style.hpp", "int editor_input_height = 28; int editor_button_height = 30;")

    def put(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def write_spec(self):
        self.put("docs/design-system/tokens.json", json.dumps(self.data))
        rows = []
        for section in ("color", "size", "spacing", "radius"):
            for entry in self.data[section]["tokens"]:
                value = entry["value"]
                values = list(value.values()) if isinstance(value, dict) else [value]
                resolved = []
                for item in values:
                    reference = next(e["value"] for e in self.data["color"]["tokens"] if e["name"] == item[1:-1]) if item.startswith("{") else item
                    assert isinstance(reference, str)
                    resolved.append(reference)
                values = resolved
                rows.append(f"| `{entry['name']}` | " + " | ".join(values) + " | use |")
        self.put("docs/DesignSystem.md", "\n".join(rows))

    def test_matching_fixture_passes(self):
        self.assertEqual([], rules.token_violations(self.root))

    def test_document_mutation_is_caught(self):
        path = self.root / "docs/DesignSystem.md"
        self.put("docs/DesignSystem.md", path.read_text(encoding="utf-8").replace("#1f1f1f", "#202020"))
        self.assertTrue(rules.token_violations(self.root))

    def test_each_platform_source_mutation_is_caught(self):
        for relative in (rules.NATIVE_THEME, rules.LINUX_THEME):
            with self.subTest(relative=relative):
                path = self.root / relative
                original = path.read_text(encoding="utf-8")
                self.put(relative, original.replace("0x1f1f1f", "0x202020"))
                self.assertTrue(rules.token_violations(self.root))
                self.put(relative, original)

    def test_shared_accent_2_mutation_is_caught(self):
        path = self.root / rules.COLOR_MATH
        original = path.read_text(encoding="utf-8")
        for old, new in (("0x60b0ff", "0x60b0fe"), ("0xb99bff", "0xb99bfe")):
            with self.subTest(old=old):
                self.put(rules.COLOR_MATH, original.replace(old, new))
                self.assertTrue(rules.token_violations(self.root))
        path.unlink()
        self.assertTrue(rules.token_violations(self.root))

    def test_json_mutation_is_caught(self):
        self.data["color"]["tokens"][0]["value"] = "#202020"
        self.write_spec()  # Even if JSON and doc are changed together.
        self.assertTrue(rules.token_violations(self.root))

    def test_derived_color_mutation_is_caught(self):
        entry = next(e for e in self.data["color"]["tokens"] if e["name"] == "selection")
        values = entry["value"]
        assert isinstance(values, dict)
        values["copper"] = "#000000"
        self.write_spec()
        self.assertTrue(rules.token_violations(self.root))

    def test_control_metric_mutation_is_caught(self):
        self.put("src/editor/linux/editor_style.hpp", "int editor_input_height = 29; int editor_button_height = 30;")
        self.assertTrue(rules.token_violations(self.root))

    def test_missing_spec_fails_closed(self):
        (self.root / "docs/design-system/tokens.json").unlink()
        self.assertTrue(rules.token_violations(self.root))

    def test_cli_missing_path_fails_closed(self):
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(1, check.main(["--root", str(self.root), "missing.cpp"]))


if __name__ == "__main__":
    unittest.main()
