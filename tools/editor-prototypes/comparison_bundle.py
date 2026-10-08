#!/usr/bin/env python3
"""Same-host comparison bundle; NOT a LayerC release/native/Flatpak package."""
import argparse
import gzip
import hashlib
import json
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("executable", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
exe = args.executable.resolve(strict=True)
if exe.name not in ("layere_proto_fltk", "layere_proto_fltk_a11y", "layere_proto_gtk"):
    raise SystemExit("only standalone prototype executables are accepted")
args.output.mkdir(parents=True, exist_ok=True)
archive = args.output / f"{exe.name}.tar.gz"
if archive.exists():
    raise SystemExit("refusing to overwrite an existing comparison bundle")
# ldd is only used on our locally built, trusted prototypes.
linked = subprocess.check_output(["ldd", str(exe)], text=True)  # noqa: S603
libraries = {}
for line in linked.splitlines():
    fields = line.split()
    if len(fields) >= 3 and fields[1] == "=>":
        if not fields[2].startswith("/"):
            raise SystemExit(f"unresolved library: {line}")
        libraries[fields[0]] = Path(fields[2]).resolve(strict=True)
manifest = {"kind": "same-host prototype comparison bundle",
            "notAReleasePackage": True, "originalExecutable": str(exe),
            "originalExecutableSha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
            "libraries": {}, "limitations": [
                "Host ELF interpreter, fonts, assets/plugins and session services are not bundled",
                "No native dependency policy, installation scripts or Flatpak permissions",
                "Only runnable on the measured host ABI; no portability claim"]}
with tempfile.TemporaryDirectory(prefix="layerc-proto-package-") as directory:
    root = Path(directory)
    (root / "bin").mkdir()
    (root / "lib").mkdir()
    binary = root / "bin" / exe.name
    shutil.copy2(exe, binary)
    subprocess.run(["strip", "--strip-unneeded", str(binary)], check=True)  # noqa: S603
    manifest["strippedExecutableBytes"] = binary.stat().st_size
    manifest["strippedExecutableSha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
    for soname, source in sorted(libraries.items()):
        target = root / "lib" / soname
        shutil.copy2(source, target)
        manifest["libraries"][soname] = {
            "source": str(source), "bytes": target.stat().st_size,
            "sha256": hashlib.sha256(target.read_bytes()).hexdigest()}
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (root / "README.txt").write_text(
        "EXPERIMENTAL same-host comparison only, not a LayerC package.\n"
        "Never launch this incomplete prototype against ambient HOME/XDG data.\n"
        "Run it only through the repository prototype measure/interaction harness,\n"
        "which creates an authenticated private display, bus and marked fixture.\n"
        "Set the bundle lib directory in LD_LIBRARY_PATH for that harness.\n"
        "The executable rejects unmarked or escaping fixtures before scanning.\n"
        "See manifest.json for omitted components and exact checksums.\n")
    with (archive.open("wb") as stream,
          gzip.GzipFile(fileobj=stream, mode="wb", filename="", mtime=0) as compressed,
          tarfile.open(fileobj=compressed, mode="w") as tar):
        for path in sorted(root.rglob("*")):
            info = tar.gettarinfo(str(path), arcname=str(path.relative_to(root)))
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            info.mtime = 0
            if path.is_file():
                with path.open("rb") as data:
                    tar.addfile(info, data)
            else:
                tar.addfile(info)
    # Verify stored files without extracting anything into a user directory.
    with tarfile.open(archive, "r:gz") as tar:
        member = tar.extractfile(f"bin/{exe.name}")
        if member is None:
            raise RuntimeError("comparison archive is missing the executable")
        with member:
            data = member.read()
        if hashlib.sha256(data).hexdigest() != manifest["strippedExecutableSha256"]:
            raise RuntimeError("comparison archive executable checksum mismatch")
report = {**manifest, "archiveBytes": archive.stat().st_size,
          "archiveSha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
          "archive": str(archive), "archiveReadbackVerified": True}
(args.output / f"{exe.name}-package.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps({k: report[k] for k in (
    "kind", "archive", "archiveBytes", "strippedExecutableBytes", "archiveReadbackVerified")}))
