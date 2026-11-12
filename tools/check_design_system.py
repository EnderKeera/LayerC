#!/usr/bin/env python3
"""Fail on mechanically decidable rules from docs/DesignSystem.md.

Usage: python tools/check_design_system.py [--root DIR] [PATH ...]
Without PATH, check both editors and their shared native chrome. Token agreement
is always checked. Rendered layout, keyboard paths and accessibility still need
the review and native tests prescribed by the design system.
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

from design_system_checks import (
    NONCODE,
    SHARED_CHROME,
    token_violations,
    visual_violations,
)

SOURCE_SUFFIXES = {".cpp", ".hpp", ".h", ".rc"}
PROPER_NOUNS = frozenset({
    "Windows", "Narrator", "Magnifier", "Orca", "Linux", "Wayland",
    "Ctrl", "Shift", "Alt", "Del", "Esc", "Enter", "Tab", "Space",
    "Up", "Down", "Home", "End", "Backspace", "Delete",
    "Graphite", "Copper", "Teal", "Plum", "Slate", "System", "Hz",
})
PROPER_PHRASES = ("LayerC Editor", "LayerC Calibration", "Raw Input", "Game Bar", "Segoe UI")
RC_TEXT = re.compile(
    r'^\s*(MENUITEM|POPUP|CAPTION|PUSHBUTTON|DEFPUSHBUTTON|LTEXT|RTEXT|CTEXT'
    r'|CONTROL|GROUPBOX|AUTOCHECKBOX|AUTORADIOBUTTON)\s+"((?:[^"]|"")*)"')
WORD = re.compile(r"[A-Za-z][A-Za-z0-9']*")
BOUNDARY = re.compile(r"(?:[.:!?—]\s+|\s/\s|\()$")
EMOJI = re.compile("[\U0001F300-\U0001FAFF☀-➿️]")
UNIT_GLUED = re.compile(r"\d(?:%|px|ms|s)\b")
CONTROL_KINDS = {"MENUITEM", "PUSHBUTTON", "DEFPUSHBUTTON", "CONTROL", "AUTOCHECKBOX"}


@dataclass(frozen=True)
class Violation:
    path: Path
    line: int
    rule: str
    message: str

    def render(self, root: Path) -> str:
        shown = self.path.relative_to(root) if self.path.is_relative_to(root) else self.path
        return f"{shown.as_posix()}:{self.line}: [{self.rule}] {self.message}"


def label_text(raw: str) -> str:
    text = raw.replace('""', '"').split("\\t", 1)[0].split("\t", 1)[0]
    return text.replace("&&", "\0").replace("&", "").replace("\0", "&")


def sentence_case_errors(text: str, control_words: frozenset[str] = frozenset()) -> list[str]:
    """Allow proper nouns, acronyms, new sentences and prose naming controls."""
    prose, masked = text.rstrip().endswith("."), text
    for phrase in PROPER_PHRASES:
        masked = masked.replace(phrase, "\0" * len(phrase))
    errors, first = [], True
    for match in WORD.finditer(masked):
        word = match.group()
        starts = first or BOUNDARY.search(masked[:match.start()]) is not None
        first = False
        if starts or not word[0].isupper() or word in PROPER_NOUNS or len(word) == 1:
            continue
        if prose and word in control_words:
            continue
        if any(ch.isupper() or ch.isdigit() for ch in word[1:]):
            continue
        errors.append(word)
    return errors


def string_literals(source: str):
    """(line, text) outside comments; understand prefixes and raw strings."""
    for match in NONCODE.finditer(source):
        literal = match.group()
        if literal.startswith(("//", "/*")) or '"' not in literal:
            continue
        quote = literal.index('"')
        if quote and literal[quote - 1] == "R":
            opening = literal.index("(", quote)
            closing = ")" + literal[quote + 1:opening] + '"'
            value = literal[opening + 1:-len(closing)]
        else:
            value = literal[quote + 1:-1]
        yield source.count("\n", 0, match.start()) + 1, value


def check_text(path: Path, number: int, text: str) -> list[Violation]:
    checks = (
        ("ellipsis", "…" in text, 'write "..." (three periods), not U+2026'),
        ("could-not", re.search(r"\bcouldn't\b", text, re.IGNORECASE), 'write "Could not", not "Couldn\'t"'),
        ("exclamation", re.search(r"!(?:\s|$|\\n)", text), "no exclamation marks"),
        ("emoji", EMOJI.search(text), "no emoji"),
        ("layerd-name", re.search(r"\b[Dd]aemon\b", text), 'the daemon is "LayerD" in everything a person reads'),
        ("unit-space", UNIT_GLUED.search(text), 'units follow a space: "6.5 s", "130 %"'),
    )
    return [Violation(path, number, rule, f'"{text}": {message}')
            for rule, bad, message in checks if bad]


def check_rc(path: Path, text: str) -> list[Violation]:
    labels = [(number, match) for number, line in enumerate(text.splitlines(), 1)
              if (match := RC_TEXT.match(line))]
    controls = frozenset(word.group() for _, match in labels
                         if match[1] in CONTROL_KINDS
                         if (word := WORD.search(label_text(match[2]))))
    found = []
    for number, match in labels:
        label = label_text(match[2])
        found.extend(Violation(path, number, "sentence-case", f'"{label}": "{word}" is capitalized mid-label')
                     for word in sentence_case_errors(label, controls))
        found.extend(check_text(path, number, label))
    return found


def check_source(path: Path, text: str) -> list[Violation]:
    found, lines = [], text.splitlines()
    for number, literal in string_literals(text):
        if lines[number - 1].lstrip().startswith("#include"):
            continue
        if re.fullmatch(r"[a-z0-9_./\\:+%-]+", literal):
            continue  # Protocol keys and include paths are not UI prose.
        found.extend(check_text(path, number, literal))
        context = "\n".join(lines[max(0, number - 3):number - 1] + [lines[number - 1].split('"', 1)[0]])
        menu_path = re.match(r"&?(?:File|Edit|View|Tools|Help|Panels|Cooldowns|Bindings)/", literal)
        menu_call = re.search(r"\b(?:AppendMenu[AW]?|InsertMenu[AW]?)\s*\([^;]*$", context)
        if menu_path or menu_call:
            for part in literal.split("/"):
                label = label_text(part)
                found.extend(Violation(path, number, "sentence-case", f'"{label}": "{word}" is capitalized mid-label')
                             for word in sentence_case_errors(label))
    return found


def check_path(path: Path, root: Path | None = None) -> list[Violation]:
    text = path.read_text(encoding="utf-8")
    if path.suffix == ".rc":
        return check_rc(path, text)
    relative = path.relative_to(root).as_posix() if root and path.is_relative_to(root) else path.as_posix()
    return check_source(path, text) + [Violation(path, *item) for item in visual_violations(text, relative)]


def editor_files(root: Path) -> list[Path]:
    files = [path for path in (root / "src/editor").rglob("*")
             if path.suffix in SOURCE_SUFFIXES and path.is_file()]
    files.extend(root / relative for relative in SHARED_CHROME)
    return sorted(set(files))


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("paths", nargs="*", type=Path)
    args = parser.parse_args(argv)
    root = args.root.resolve()
    files = [path if path.is_absolute() else root / path for path in args.paths] or editor_files(root)
    try:
        violations = [Violation(*item) for item in token_violations(root)]
        violations.extend(violation for path in files for violation in check_path(path, root))
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"design-system check could not complete: {error}", file=sys.stderr)
        return 1
    for violation in violations:
        print(violation.render(root))
    if violations:
        print(f"{len(violations)} design-system violation(s); see docs/DesignSystem.md.")
        return 1
    print(f"Design system check passed ({len(files)} files).")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
