#!/usr/bin/env python3
"""Check a built LayerC Debian package before anything installs it.

The control fields must carry the configured version and, when the package
ships the editor, the sd-bus runtime of its own AT-SPI bridge, no ATK or
libatspi, and a recommendation of the accessibility bus; the payload must hold
every program, the third-party notices and, for the editor, a valid desktop
entry, icon set and AppStream component. Writes a JSON report and exits non-zero on failure.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

PROGRAMS = ("layerd", "layerctl", "layercal")
NOTICES = (
    "usr/share/licenses/LayerC/THIRD_PARTY_NOTICES.md",
    "usr/share/licenses/LayerC/miniz/LICENSE",
    "usr/share/licenses/LayerC/miniz/UPSTREAM.md",
    "usr/share/licenses/LayerC/fltk/COPYING",
    "usr/share/licenses/LayerC/fltk/UPSTREAM.md",
)
APP_ID = "io.github.EnderKeera.LayerC"
# The release tree enables the optional user unit; it must run the packaged
# daemon, never a build-tree path.
USER_UNIT = f"usr/lib/systemd/user/{APP_ID}.service"
EDITOR_FILES = (
    "usr/bin/layere",
    f"usr/share/applications/{APP_ID}.desktop",
    f"usr/share/metainfo/{APP_ID}.metainfo.xml",
    *(f"usr/share/icons/hicolor/{size}x{size}/apps/{APP_ID}.png" for size in (48, 64, 128, 256)),
)
# The editor speaks AT-SPI over sd-bus itself: libsystemd (or libelogind)
# carries the bridge, ATK and libatspi must not return, and at-spi2-core
# provides the accessibility bus screen readers use.
BRIDGE_RUNTIMES = ("libsystemd0", "libelogind0")
RETIRED_ACCESSIBILITY = ("libatk1.0", "libatk-bridge2.0", "libatspi2.0")
ACCESSIBILITY_BUS = "at-spi2-core"


def run(*command: str) -> str:
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout


def check(deb: Path, version: str, editor: bool) -> dict[str, object]:
    failures: list[str] = []
    fields = {
        name: run("dpkg-deb", "-f", str(deb), name).strip()
        for name in ("Package", "Version", "Architecture", "Depends", "Recommends",
                     "Installed-Size", "Maintainer", "Description")
    }
    if "installer created using CPack" in fields["Description"]:
        failures.append("Description is CPack's generic placeholder")
    if fields["Package"] != "layerc":
        failures.append(f"package name {fields['Package']!r} is not 'layerc'")
    if fields["Version"] != f"{version}-1":
        failures.append(f"version {fields['Version']!r} does not match configured {version}-1")
    depends = [item.strip() for item in fields["Depends"].split(",") if item.strip()]
    recommends = [item.strip() for item in fields["Recommends"].split(",") if item.strip()]
    if editor:
        if not any(item.split()[0] in BRIDGE_RUNTIMES for item in depends):
            failures.append("Depends lacks the sd-bus runtime of the AT-SPI bridge")
        failures += [f"Depends names retired {item}" for item in depends
                     if item.split()[0].startswith(RETIRED_ACCESSIBILITY)]
        if ACCESSIBILITY_BUS not in (item.split()[0] for item in recommends):
            failures.append(f"Recommends lacks {ACCESSIBILITY_BUS}")
        if not any(item.startswith("libfltk1.4") for item in depends):
            failures.append("Depends lacks the FLTK 1.4 runtime")

    with tempfile.TemporaryDirectory(prefix="layerc-deb-check.") as scratch:
        root = Path(scratch)
        run("dpkg-deb", "-x", str(deb), str(root))
        expected = [f"usr/bin/{name}" for name in PROGRAMS] + list(NOTICES) + [USER_UNIT]
        if editor:
            expected += EDITOR_FILES
        failures += [f"payload lacks /{path}" for path in expected if not (root / path).is_file()]
        failures += [f"notice /{path} is empty" for path in NOTICES
                     if (root / path).is_file() and (root / path).stat().st_size == 0]
        outside = sorted(
            str(path.relative_to(root)) for path in root.rglob("*")
            if not path.is_dir() and path.relative_to(root).parts[0] != "usr"
        )
        failures += [f"payload installs /{path} outside /usr" for path in outside]
        if (root / "usr/local").exists():
            failures.append("payload installs under /usr/local")
        unit = root / USER_UNIT
        if unit.is_file() and "ExecStart=/usr/bin/layerd\n" not in unit.read_text(encoding="utf-8"):
            failures.append("the user unit does not start /usr/bin/layerd")
        if not editor and (root / "usr/bin/layere").exists():
            failures.append("payload ships layere although the editor was not expected")
        validation: dict[str, str] = {}
        if editor and not failures:
            for name, command in (
                ("desktop-file-validate",
                 ["desktop-file-validate", str(root / EDITOR_FILES[1])]),
                ("appstreamcli",
                 ["appstreamcli", "validate", "--no-net", str(root / EDITOR_FILES[2])]),
            ):
                result = subprocess.run(command, capture_output=True, text=True)
                validation[name] = (result.stdout + result.stderr).strip()
                if result.returncode != 0:
                    failures.append(f"{name} rejected the installed metadata")

    return {
        "package": deb.name,
        "fields": fields,
        "editor": editor,
        "validation": validation,
        "failures": failures,
        "result": "fail" if failures else "pass",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("deb", type=Path)
    parser.add_argument("--version", required=True, help="configured PROJECT_VERSION")
    parser.add_argument("--editor", choices=("yes", "no"), required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    report = check(args.deb, args.version, args.editor == "yes")
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if report["result"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
