#!/usr/bin/env python3
"""Prepare an offline, locally signed Windows updater release directory."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from typing import Any


_PUBLIC_KEY_DER_PREFIX = bytes.fromhex(
    "3059301306072a8648ce3d020106082a8648ce3d030107034200"
)
_CHUNK_BYTES = 1024 * 1024
_MAX_PUBLIC_KEY_TEXT_BYTES = 4096
_MAX_MANIFEST_BYTES = 256 * 1024
_MAX_FILE_BYTES = 256 * 1024 * 1024
_MAX_SIGNATURE_BYTES = 1024


class ReleasePreparationError(Exception):
    pass


def _is_within(path: Path, directory: Path) -> bool:
    return path == directory or directory in path.parents


def _read_bounded(path: Path, maximum: int, description: str) -> bytes:
    contents = bytearray()
    try:
        with path.open("rb") as stream:
            while len(contents) <= maximum:
                chunk = stream.read(min(_CHUNK_BYTES, maximum + 1 - len(contents)))
                if not chunk:
                    break
                contents.extend(chunk)
    except OSError as error:
        raise ReleasePreparationError(f"could not read {description}") from error
    if len(contents) > maximum:
        raise ReleasePreparationError(f"{description} exceeds its size limit")
    return bytes(contents)


def _run(command: list[str], description: str) -> None:
    try:
        result = subprocess.run(
            command,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except (OSError, subprocess.SubprocessError) as error:
        raise ReleasePreparationError(f"could not run {description}") from error
    if result.returncode != 0:
        raise ReleasePreparationError(f"{description} failed")


def _load_public_point(path: Path) -> bytes:
    text = _read_bounded(path, _MAX_PUBLIC_KEY_TEXT_BYTES, "public key")
    try:
        public_hex = text.decode("ascii").strip(" \t\r\n")
    except UnicodeDecodeError as error:
        raise ReleasePreparationError("public key is not ASCII hex") from error
    if not re.fullmatch(r"04[0-9a-fA-F]{128}", public_hex):
        raise ReleasePreparationError("public key must be 130 hex characters beginning with 04")
    return bytes.fromhex(public_hex)


def _derive_public_key(
    openssl: str, private_key: Path, scratch: Path, pinned_point: bytes
) -> Path:
    public_der = scratch / "public.der"
    _run(
        [
            openssl,
            "pkey",
            "-in",
            str(private_key),
            "-passin",
            "pass:",
            "-pubout",
            "-outform",
            "DER",
            "-out",
            str(public_der),
        ],
        "private key public-point derivation",
    )
    der = _read_bounded(public_der, len(_PUBLIC_KEY_DER_PREFIX) + 65, "derived public key")
    if (
        len(der) != len(_PUBLIC_KEY_DER_PREFIX) + 65
        or not der.startswith(_PUBLIC_KEY_DER_PREFIX)
        or der[len(_PUBLIC_KEY_DER_PREFIX)] != 0x04
    ):
        raise ReleasePreparationError("derived key is not the expected P-256 SPKI encoding")
    if der[len(_PUBLIC_KEY_DER_PREFIX) :] != pinned_point:
        raise ReleasePreparationError("private key does not match the supplied public key")

    public_pem = scratch / "public.pem"
    _run(
        [
            openssl,
            "pkey",
            "-pubin",
            "-inform",
            "DER",
            "-in",
            str(public_der),
            "-pubout",
            "-out",
            str(public_pem),
        ],
        "temporary public-key conversion",
    )
    return public_pem


def _manifest_files(manifest: bytes) -> list[dict[str, Any]]:
    try:
        parsed = json.loads(manifest.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ReleasePreparationError("native manifest output is not valid UTF-8 JSON") from error
    if not isinstance(parsed, dict) or not isinstance(parsed.get("files"), list):
        raise ReleasePreparationError("native manifest output has no file list")

    files: list[dict[str, Any]] = []
    for entry in parsed["files"]:
        if not isinstance(entry, dict) or set(entry) != {"path", "module", "size", "sha256"}:
            raise ReleasePreparationError("native manifest contains a malformed file entry")
        path = entry["path"]
        module = entry["module"]
        size = entry["size"]
        digest = entry["sha256"]
        if not isinstance(path, str) or not isinstance(module, str):
            raise ReleasePreparationError("native manifest contains a malformed file entry")
        if isinstance(size, bool) or not isinstance(size, int) or not 0 <= size <= _MAX_FILE_BYTES:
            raise ReleasePreparationError("native manifest contains an invalid file size")
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ReleasePreparationError("native manifest contains an invalid file hash")
        segments = path.split("/")
        if (
            len(segments) > 4
            or any(
                not segment
                or segment.startswith(".")
                or segment.endswith(".")
                or not re.fullmatch(r"[A-Za-z0-9._-]+", segment)
                for segment in segments
            )
        ):
            raise ReleasePreparationError("native manifest contains an unsafe file path")
        files.append(entry)
    if not files:
        raise ReleasePreparationError("native manifest lists no payload files")
    return files


def _payload_path(root: Path, relative: str) -> Path:
    source = root
    parts = relative.split("/")
    try:
        for index, part in enumerate(parts):
            source = source / part
            metadata = source.lstat()
            if stat.S_ISLNK(metadata.st_mode):
                raise ReleasePreparationError("payload contains a symlink")
            if index < len(parts) - 1 and not stat.S_ISDIR(metadata.st_mode):
                raise ReleasePreparationError("payload path is not a regular file path")
        if not stat.S_ISREG(source.lstat().st_mode):
            raise ReleasePreparationError("payload entry is not a regular file")
        resolved = source.resolve(strict=True)
    except ReleasePreparationError:
        raise
    except OSError as error:
        raise ReleasePreparationError("payload file is unavailable") from error
    if not _is_within(resolved, root):
        raise ReleasePreparationError("payload file resolves outside the payload root")
    return source


def _hash_file(path: Path, expected_size: int, expected_hash: str) -> None:
    digest = hashlib.sha256()
    size = 0
    try:
        with path.open("rb") as stream:
            while True:
                chunk = stream.read(_CHUNK_BYTES)
                if not chunk:
                    break
                size += len(chunk)
                if size > expected_size:
                    raise ReleasePreparationError("payload changed after manifest generation")
                digest.update(chunk)
    except ReleasePreparationError:
        raise
    except OSError as error:
        raise ReleasePreparationError("could not read staged payload") from error
    if size != expected_size or digest.hexdigest() != expected_hash:
        raise ReleasePreparationError("payload changed after manifest generation")


def _copy_and_verify(source: Path, destination: Path, expected_size: int, expected_hash: str) -> None:
    digest = hashlib.sha256()
    size = 0
    try:
        with source.open("rb") as source_stream, destination.open("xb") as destination_stream:
            while True:
                chunk = source_stream.read(_CHUNK_BYTES)
                if not chunk:
                    break
                size += len(chunk)
                if size > expected_size:
                    raise ReleasePreparationError("payload changed after manifest generation")
                destination_stream.write(chunk)
                digest.update(chunk)
    except ReleasePreparationError:
        raise
    except OSError as error:
        raise ReleasePreparationError("could not copy payload into staging") from error
    if size != expected_size or digest.hexdigest() != expected_hash:
        raise ReleasePreparationError("payload changed after manifest generation")
    _hash_file(destination, expected_size, expected_hash)


def prepare_release(
    *,
    root: Path,
    manifest_tool: Path,
    private_key: Path,
    public_key: Path,
    output: Path,
    channel: str,
    sequence: int,
    commit: str,
    openssl: str = "openssl",
) -> Path:
    try:
        payload_root = Path(root).resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise ReleasePreparationError("payload root does not exist") from error
    if not payload_root.is_dir():
        raise ReleasePreparationError("payload root is not a directory")

    requested_output = Path(output)
    final_output = requested_output.resolve(strict=False)
    if requested_output.exists() or requested_output.is_symlink() or final_output.exists():
        raise ReleasePreparationError("final output already exists")
    if _is_within(final_output, payload_root):
        raise ReleasePreparationError("final output must be outside the payload root")
    if not final_output.parent.is_dir():
        raise ReleasePreparationError("final output parent must already exist")

    try:
        manifest_executable = Path(manifest_tool).resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise ReleasePreparationError("manifest tool is unavailable") from error
    if not manifest_executable.is_file():
        raise ReleasePreparationError("manifest tool is not a file")

    try:
        private_path = Path(private_key).resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise ReleasePreparationError("private key is unavailable") from error
    if not private_path.is_file():
        raise ReleasePreparationError("private key is not a file")
    if _is_within(private_path, payload_root):
        raise ReleasePreparationError("private key must be outside the payload root")

    try:
        public_path = Path(public_key).resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise ReleasePreparationError("public key is unavailable") from error
    pinned_point = _load_public_point(public_path)

    try:
        stage = Path(
            tempfile.mkdtemp(
                prefix=f".{final_output.name}.staging-",
                dir=final_output.parent,
            )
        )
    except OSError as error:
        raise ReleasePreparationError("could not create staging directory") from error
    try:
        with tempfile.TemporaryDirectory(
            prefix=f".{final_output.name}.scratch-",
            dir=final_output.parent,
            ignore_cleanup_errors=True,
        ) as scratch_directory:
            scratch = Path(scratch_directory)
            public_pem = _derive_public_key(openssl, private_path, scratch, pinned_point)

            manifest_path = scratch / "manifest.json"
            _run(
                [
                    str(manifest_executable),
                    "--root",
                    str(payload_root),
                    "--channel",
                    channel,
                    "--sequence",
                    str(sequence),
                    "--commit",
                    commit,
                    "--output",
                    str(manifest_path),
                ],
                "native manifest generation",
            )
            manifest_bytes = _read_bounded(manifest_path, _MAX_MANIFEST_BYTES, "manifest")
            entries = _manifest_files(manifest_bytes)
            manifest_destination = stage / "manifest.json"
            try:
                with manifest_destination.open("xb") as stream:
                    stream.write(manifest_bytes)
            except OSError as error:
                raise ReleasePreparationError("could not stage manifest") from error

            staged_hashes: dict[str, int] = {}
            for entry in entries:
                source = _payload_path(payload_root, entry["path"])
                digest = entry["sha256"]
                size = entry["size"]
                previous_size = staged_hashes.get(digest)
                if previous_size is not None:
                    if previous_size != size:
                        raise ReleasePreparationError("duplicate payload hash has inconsistent sizes")
                    _hash_file(source, size, digest)
                    continue
                _copy_and_verify(source, stage / digest, size, digest)
                staged_hashes[digest] = size

            signature_path = stage / "manifest.sig"
            _run(
                [
                    openssl,
                    "dgst",
                    "-sha256",
                    "-sign",
                    str(private_path),
                    "-passin",
                    "pass:",
                    "-out",
                    str(signature_path),
                    str(manifest_destination),
                ],
                "manifest signing",
            )
            try:
                signature_size = signature_path.stat().st_size
            except OSError as error:
                raise ReleasePreparationError("signature output is unavailable") from error
            if signature_size == 0 or signature_size > _MAX_SIGNATURE_BYTES:
                raise ReleasePreparationError("signature output has an invalid size")
            _run(
                [
                    openssl,
                    "dgst",
                    "-sha256",
                    "-verify",
                    str(public_pem),
                    "-signature",
                    str(signature_path),
                    str(manifest_destination),
                ],
                "local signature verification",
            )

        if requested_output.exists() or requested_output.is_symlink() or final_output.exists():
            raise ReleasePreparationError("final output appeared before publication")
        try:
            os.rename(stage, final_output)
        except OSError as error:
            raise ReleasePreparationError("could not publish staged release") from error
        return final_output
    except ReleasePreparationError:
        raise
    except OSError as error:
        raise ReleasePreparationError("local release staging failed") from error
    finally:
        shutil.rmtree(stage, ignore_errors=True)


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--manifest-tool", type=Path, required=True)
    parser.add_argument("--private-key", type=Path, required=True)
    parser.add_argument("--public-key", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--channel", required=True)
    parser.add_argument("--sequence", type=int, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--openssl", default="openssl")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)
    try:
        output = prepare_release(
            root=args.root,
            manifest_tool=args.manifest_tool,
            private_key=args.private_key,
            public_key=args.public_key,
            output=args.output,
            channel=args.channel,
            sequence=args.sequence,
            commit=args.commit,
            openssl=args.openssl,
        )
    except ReleasePreparationError as error:
        print(f"prepare_update_release: {error}", file=sys.stderr)
        return 1
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
