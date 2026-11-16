"""Mechanical editor chrome rules used by check_design_system.py.

This is a source-level guard, not a C++ parser or a replacement for rendering,
keyboard and accessibility tests. Keep exceptions scoped to semantic sources.
"""
from __future__ import annotations

import json
import re
from pathlib import Path

NATIVE_THEME = "src/platform/win32/include/layerc/platform/native_theme.hpp"
LINUX_THEME = "src/editor/fltk/editor_appearance.cpp"
# The color math both editors share, with the two `accent_2` values.
COLOR_MATH = "src/editor/common/editor_color.hpp"
TOKEN_SOURCES = frozenset({NATIVE_THEME, LINUX_THEME, "src/platform/win32/native_theme.cpp",
                           COLOR_MATH, "src/editor/common/editor_color.cpp"})
SHARED_CHROME = (NATIVE_THEME, "src/platform/win32/native_theme.cpp",
                 "src/platform/win32/native_controls.cpp")
METRIC_SOURCES = frozenset({"src/editor/dialog_layout.hpp", "src/editor/fltk/editor_style.hpp"})
# These modules render a profile's overlay, not editor chrome. Only rounding
# (not arbitrary colors/metrics) is exempt here.
OVERLAY_PREVIEWS = frozenset({"src/editor/fltk/editor_preview.cpp", "src/editor/editor_preview_render.cpp"})
NONCODE = re.compile(
    r'//[^\n]*|/\*.*?\*/|(?:u8|u|U|L)?R"(?P<delim>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delim)"'
    r'|(?:u8|u|U|L)?"(?:\\.|[^"\\])*"|(?:u|U|L)?\'(?:\\.|[^\'\\\n])\'', re.DOTALL)


def code_only(text: str) -> str:
    """Blank comments/literals without changing offsets or newlines."""
    return NONCODE.sub(lambda m: re.sub(r"[^\n]", " ", m.group()), text)


def arguments(code: str, opening: int) -> list[str]:
    depth, start = 0, opening + 1
    result = []
    for pos in range(start, len(code)):
        char = code[pos]
        if char in "([{":
            depth += 1
        elif char in ")]}":
            if char == ")" and depth == 0:
                return result + [code[start:pos].strip()]
            depth -= 1
        elif char == "," and depth == 0:
            result.append(code[start:pos].strip())
            start = pos + 1
    return []


def preview_spans(code: str) -> list[tuple[int, int]]:
    """Windows preview functions can share a TU with normal dialog painters."""
    result = []
    for match in re.finditer(r"\b(?:draw_bar_preview|draw_preview|draw_actual_preview|paint_overlay_preview)\s*\(", code):
        body = code.find("{", match.end())
        if body < 0 or ";" in code[match.end():body]:
            continue
        depth, end = 1, body + 1
        while end < len(code) and depth:
            depth += (code[end] == "{") - (code[end] == "}")
            end += 1
        result.append((match.start(), end))
    return result


def visual_violations(text: str, relative: str) -> list[tuple[int, str, str]]:
    code, found = code_only(text), []

    def add(offset: int, rule: str, message: str) -> None:
        found.append((text.count("\n", 0, offset) + 1, rule, message))

    if relative not in TOKEN_SOURCES:
        channel = r"(?:0x[0-9a-fA-F]+|\d+)[uUlL]*"
        colors = rf"\b0x[0-9a-fA-F]{{6}}[uUlL]*\b|\b(?:RGB|fl_rgb_color)\s*\(\s*{channel}\s*,\s*{channel}\s*,\s*{channel}\s*\)"
        for match in re.finditer(colors, code):
            start, end = code.rfind("\n", 0, match.start()) + 1, code.find("\n", match.end())
            line = code[start:end if end >= 0 else len(code)]
            # A color format's maximum/mask is data validation, not paint.
            if re.fullmatch(r"0x[fF]{6}[uUlL]*", match.group()) and re.search(r"(?:&|<=|>=|(?<![-])>|<)\s*0x[fF]{6}", line):
                continue
            # Profile capture predicates and transparency masks are data, not
            # chrome paint. Restrict the exception to the literal's own API.
            prefix = code[start:match.start()]
            if re.search(r"(?:\bAverageColorPredicate\s*\{|\bImageList_AddMasked\s*\([^;]*,\s*)$", prefix):
                continue
            add(match.start(), "token-color", "hard-coded color outside the token source; use a theme/role token")
        fixed = r"\bFL_(?:BLACK|WHITE|RED|GREEN|BLUE|YELLOW|CYAN|MAGENTA|DARK_RED|DARK_GREEN|DARK_BLUE|DARK_YELLOW|DARK_CYAN|DARK_MAGENTA|GRAY|DARK1|DARK2|DARK3|LIGHT1|LIGHT2|LIGHT3)\b"
        for match in re.finditer(fixed, code):
            add(match.start(), "token-color", "fixed FLTK color outside the token source; use a theme/role token")

    if relative not in OVERLAY_PREVIEWS:
        spans = preview_spans(code) if relative == "src/editor/editor_draw.cpp" else []
        rounded = r"\b(?:RoundRect|CreateRoundRectRgn|FL_(?:ROUNDED|RSHADOW|RFLAT|ROUND_UP|ROUND_DOWN|OVAL|OSHADOW|OFLAT)_BOX|FL_(?:ROUNDED|ROUND|OVAL)_FRAME)\b"
        for match in re.finditer(rounded, code):
            if not any(start <= match.start() < end for start, end in spans):
                add(match.start(), "square-chrome", "rounded editor chrome; only overlay previews follow corner_radius")

    widget = r"\bnew\s+(?:(?:EditorField|FocusedWidget)\s*<\s*)?(?:Fl_(?:Input|Int_Input|Float_Input|Output|Choice|Button|Return_Button|Light_Button)|EditorChoice|CommandButton)\s*>?\s*\("
    for match in re.finditer(widget, code):
        args = arguments(code, match.end() - 1)
        if len(args) >= 4 and re.fullmatch(r"\d+", args[3]):
            add(match.start(), "control-height", "use editor_input_height/editor_button_height, not a literal control height")
    for match in re.finditer(r"\bCreateWindow(Ex)?[AW]?\s*\(", code):
        args = arguments(code, match.end() - 1)
        height = 7 if match.group(1) else 6
        if len(args) <= height or not re.fullmatch(r"[1-9]\d*", args[height]):
            continue
        # Class strings are blank in code, so look at the original call.
        end = match.end() + 1
        commas = 2 if match.group(1) else 1
        for _ in range(commas):
            end = code.find(",", end) + 1
        if re.search(r'"(?:BUTTON|EDIT|COMBOBOX)"', text[match.end():end], re.IGNORECASE):
            add(match.start(), "control-height", "use DialogLayout/native_control_height, not a literal control height")
    if relative not in METRIC_SOURCES:
        for match in re.finditer(r"\b(?:control_height|control_h|button_height|button_h|edit_height|edit_h)\s*=\s*\d+\b", code):
            add(match.start(), "control-height", "control metrics belong in the shared metric source")
    return found


def blend_rgb(base: int, over: int, amount: int) -> int:
    # C++ integer division truncates toward zero (not Python's negative //).
    return sum((((base >> shift) & 255) + int((((over >> shift) & 255) - ((base >> shift) & 255)) * amount / 255)) << shift
               for shift in (16, 8, 0))


def token_violations(root: Path) -> list[tuple[Path, int, str, str]]:
    """Check published tables/JSON, fixed ramps and single-source row metrics.

    Native C++ tests cover actual derived-color wiring and contrast floors;
    here the published derived values are recomputed from the fixed ramp.
    Missing sources/tokens fail closed.
    """
    found = []
    doc_path, json_path = root / "docs/DesignSystem.md", root / "docs/design-system/tokens.json"
    try:
        document = doc_path.read_text(encoding="utf-8")
        data = json.loads(json_path.read_text(encoding="utf-8"))
        tokens = {entry["name"]: entry["value"] for section in ("color", "size", "spacing", "radius")
                  for entry in data[section]["tokens"]}
    except (OSError, ValueError, KeyError, TypeError) as error:
        return [(json_path, 1, "token-agreement", f"cannot read required specification: {error}")]

    def mismatch(path: Path, name: str, actual, expected) -> None:
        found.append((path, 1, "token-agreement", f"{name}: found {actual!r}; expected {expected!r}"))

    def resolve(value):
        if isinstance(value, str) and value.startswith("{") and value.endswith("}"):
            return tokens[value[1:-1]]
        return value

    rows = {match[1]: [cell.strip().lower() for cell in match[2].split("|")]
            for match in re.finditer(r"^\|\s*`([^`]+)`\s*\|([^\n]+)", document, re.MULTILINE)}
    presets = ("copper", "teal", "plum", "slate")
    for name, value in tokens.items():
        expected = [str(resolve(value[preset])).lower() for preset in presets] if isinstance(value, dict) else [str(value).lower()]
        actual = rows.get(name, [])[:len(expected)]
        if actual != expected:
            mismatch(doc_path, name, actual, expected)
    for group in data.get("type", {}).get("groups", []):
        for style in group["styles"]:
            expected = [style["fontSize"].lower(), str(style["fontWeight"])]
            if rows.get(style["name"], [])[:2] != expected:
                mismatch(doc_path, style["name"], rows.get(style["name"], []), expected)

    fixed = {name: int(value[1:], 16) for name, value in tokens.items()
             if isinstance(value, str) and re.fullmatch(r"#[0-9a-fA-F]{6}", value)}
    neutral = ("background", "panel", "input", "border", "divider", "text", "secondary", "subtext", "verbose",
               "disabled", "button_hover", "row_hover", "pressed", "yes", "no", "warning", "on_status")
    for relative in (NATIVE_THEME, LINUX_THEME):
        path = root / relative
        try:
            source = code_only(path.read_text(encoding="utf-8"))
        except OSError as error:
            found.append((path, 1, "token-agreement", str(error)))
            continue
        fields = {name: int(value, 16) for name, value in re.findall(r"\.([a-z_]+)\s*=\s*(0x[0-9a-fA-F]+)", source)}
        for name in neutral:
            if fields.get(name) != fixed.get(name):
                mismatch(path, name, fields.get(name), fixed.get(name))
        for preset in presets:
            match = re.search(rf"\b{preset}_accent\s*=\s*(0x[0-9a-fA-F]+)", source)
            actual = int(match[1], 16) if match else None
            if actual != fixed.get(f"preset-{preset}"):
                mismatch(path, f"preset-{preset}", actual, fixed.get(f"preset-{preset}"))
    path = root / COLOR_MATH
    try:
        source = code_only(path.read_text(encoding="utf-8"))
    except OSError as error:
        found.append((path, 1, "token-agreement", str(error)))
        source = ""
    for color, preset in (("blue", "copper"), ("violet", "teal")):
        match = re.search(rf"\baccent_2_{color}\s*=\s*(0x[0-9a-fA-F]+)", source)
        expected = int(tokens["accent_2"][preset][1:], 16)
        if match is None or int(match[1], 16) != expected:
            mismatch(path, f"accent_2_{color}", match[1] if match else None, expected)

    for preset in presets:
        accent = fixed.get(f"preset-{preset}")
        if accent is None:
            continue
        for name, base, over, amount in (("selection", fixed["background"], accent, 60),
                                        ("primary_hover", accent, fixed["text"], 40),
                                        ("primary_pressed", accent, fixed["panel"], 60)):
            expected = f"#{blend_rgb(base, over, amount):06x}"
            if tokens[name].get(preset, "").lower() != expected:
                mismatch(json_path, f"{name}.{preset}", tokens[name].get(preset), expected)
        r, g, b = ((accent >> shift) & 255 for shift in (16, 8, 0))
        expected = "#141414" if 299*r + 587*g + 114*b >= 140000 else "#ffffff"
        if tokens["on_accent"].get(preset) != expected:
            mismatch(json_path, f"on_accent.{preset}", tokens["on_accent"].get(preset), expected)
    for name, fill in (("linux-outline-background", "background"), ("linux-outline-panel", "panel")):
        expected = f"#{blend_rgb(fixed[fill], 0xffffff, 91):06x}"
        if tokens.get(name) != expected:
            mismatch(json_path, name, tokens.get(name), expected)

    metrics = (("src/editor/dialog_layout.hpp", "native_control_height", "win-control-height"),
               ("src/editor/fltk/editor_style.hpp", "editor_input_height", "linux-input-height"),
               ("src/editor/fltk/editor_style.hpp", "editor_button_height", "linux-button-height"))
    for relative, constant, name in metrics:
        path = root / relative
        try:
            source = code_only(path.read_text(encoding="utf-8"))
            match = re.search(rf"\b{constant}\s*=\s*(\d+)", source)
            actual = f"{match[1]}px" if match else None
        except OSError:
            actual = None
        if actual != tokens.get(name):
            mismatch(path, name, actual, tokens.get(name))
    return found
