#!/usr/bin/env python3
"""Exercise offline release preparation and the production Windows verifier."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "tools"))
import prepare_update_release as release_preparation  # noqa: E402


COMMIT = "0123456789abcdef0123456789abcdef01234567"
CHANNEL = "rolling"
SEQUENCE = 27
SPKI_PREFIX = bytes.fromhex(
    "3059301306072a8648ce3d020106082a8648ce3d030107034200"
)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest-tool", type=Path, required=True)
    parser.add_argument("--verifier", type=Path, required=True)
    parser.add_argument("--openssl", required=True)
    return parser.parse_args()


def run_quiet(command: list[str]) -> subprocess.CompletedProcess[bytes]:
    try:
        result = subprocess.run(
            command,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except (OSError, subprocess.SubprocessError) as error:
        raise RuntimeError("a required offline test tool could not be run") from error
    return result


class UpdateReleaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        args = parse_arguments()
        cls.manifest_tool = args.manifest_tool.resolve(strict=True)
        cls.verifier = args.verifier.resolve(strict=True)
        cls.openssl = args.openssl
        temp_root = REPO_ROOT / "out" / "release-readiness"
        temp_root.mkdir(parents=True, exist_ok=True)
        cls.workspace = tempfile.TemporaryDirectory(
            prefix="layerc-update-signing-tests-", dir=temp_root
        )
        cls.workspace_path = Path(cls.workspace.name)
        key_directory = cls.workspace_path / "keys"
        key_directory.mkdir()
        cls.private_keys: list[Path] = []
        cls.public_keys: list[Path] = []
        for index in range(2):
            private_key = key_directory / f"disposable-{index}.pem"
            public_der = key_directory / f"disposable-{index}.der"
            public_key = key_directory / f"disposable-{index}.hex"
            generated = run_quiet(
                [
                    cls.openssl,
                    "ecparam",
                    "-name",
                    "prime256v1",
                    "-genkey",
                    "-noout",
                    "-out",
                    str(private_key),
                ]
            )
            if generated.returncode != 0:
                raise RuntimeError("OpenSSL could not create disposable test keys")
            derived = run_quiet(
                [
                    cls.openssl,
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
                ]
            )
            if derived.returncode != 0:
                raise RuntimeError("OpenSSL could not derive disposable test public keys")
            der = public_der.read_bytes()
            if len(der) != len(SPKI_PREFIX) + 65 or not der.startswith(SPKI_PREFIX):
                raise RuntimeError("OpenSSL emitted an unexpected disposable P-256 key")
            public_key.write_text(der[len(SPKI_PREFIX) :].hex() + "\n", encoding="ascii")
            cls.private_keys.append(private_key)
            cls.public_keys.append(public_key)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.workspace.cleanup()

    def new_case(self) -> tuple[Path, Path, Path]:
        case = self.workspace_path / f"case-{len(list(self.workspace_path.glob('case-*')))}"
        case.mkdir()
        payload = case / "payload"
        (payload / "licenses").mkdir(parents=True)
        (payload / "docs").mkdir()
        (payload / "layerd.exe").write_bytes(b"disposable layerd payload\x00\x01")
        (payload / "licenses" / "LICENSE.txt").write_text(
            "Disposable nested license fixture.\n", encoding="utf-8"
        )
        duplicate = b"same content-addressed test asset\n"
        (payload / "docs" / "copy-a.txt").write_bytes(duplicate)
        (payload / "docs" / "copy-b.txt").write_bytes(duplicate)
        output = case / "release-output"
        return case, payload, output

    def run_helper(
        self,
        payload: Path,
        output: Path,
        *,
        private_key: Path | None = None,
        public_key: Path | None = None,
        include_private_key: bool = True,
        manifest_tool: Path | None = None,
    ) -> subprocess.CompletedProcess[bytes]:
        command = [
            sys.executable,
            str(REPO_ROOT / "tools" / "prepare_update_release.py"),
            "--root",
            str(payload),
            "--manifest-tool",
            str(manifest_tool or self.manifest_tool),
            "--public-key",
            str(public_key or self.public_keys[0]),
            "--output",
            str(output),
            "--channel",
            CHANNEL,
            "--sequence",
            str(SEQUENCE),
            "--commit",
            COMMIT,
            "--openssl",
            self.openssl,
        ]
        if include_private_key:
            command.extend(["--private-key", str(private_key or self.private_keys[0])])
        return run_quiet(command)

    def run_manifest(self, payload: Path, output: Path) -> bytes:
        result = run_quiet(
            [
                str(self.manifest_tool),
                "--root",
                str(payload),
                "--channel",
                CHANNEL,
                "--sequence",
                str(SEQUENCE),
                "--commit",
                COMMIT,
                "--output",
                str(output),
            ]
        )
        self.assertEqual(result.returncode, 0, "native manifest generation failed")
        return output.read_bytes()

    def run_verifier(
        self, public_key: Path, manifest: Path, signature: Path
    ) -> subprocess.CompletedProcess[bytes]:
        return run_quiet([str(self.verifier), str(public_key), str(manifest), str(signature)])

    def test_signed_release_is_byte_stable_content_addressed_and_cng_valid(self) -> None:
        case, payload, output = self.new_case()
        reference = case / "reference-manifest.json"
        expected_manifest = self.run_manifest(payload, reference)

        prepared = self.run_helper(payload, output)
        self.assertEqual(prepared.returncode, 0, "release preparation failed")
        self.assertTrue(output.is_dir())
        manifest_path = output / "manifest.json"
        signature_path = output / "manifest.sig"
        self.assertEqual(manifest_path.read_bytes(), expected_manifest)
        self.assertEqual(self.run_verifier(self.public_keys[0], manifest_path, signature_path).returncode, 0)

        manifest = json.loads(manifest_path.read_bytes().decode("utf-8"))
        entries = manifest["files"]
        hashes = {entry["sha256"] for entry in entries}
        self.assertEqual(
            {path.name for path in output.iterdir()},
            {"manifest.json", "manifest.sig", *hashes},
        )
        self.assertIn("licenses/LICENSE.txt", {entry["path"] for entry in entries})
        duplicate_hashes = {
            entry["sha256"]
            for entry in entries
            if entry["path"] in {"docs/copy-a.txt", "docs/copy-b.txt"}
        }
        self.assertEqual(len(duplicate_hashes), 1)
        for entry in entries:
            asset = output / entry["sha256"]
            contents = asset.read_bytes()
            self.assertEqual(len(contents), entry["size"])
            self.assertEqual(hashlib.sha256(contents).hexdigest(), entry["sha256"])
        self.assertFalse(any(path.suffix.lower() in {".pem", ".der", ".hex"} for path in output.iterdir()))

    def test_native_verifier_exit_statuses(self) -> None:
        wrong_count = run_quiet([str(self.verifier)])
        self.assertEqual(wrong_count.returncode, 64)
        case = self.workspace_path / "case-verifier-inputs"
        case.mkdir()
        malformed = run_quiet(
            [
                str(self.verifier),
                str(self.public_keys[0]),
                str(case / "missing-manifest.json"),
                str(case / "missing-manifest.sig"),
            ]
        )
        self.assertEqual(malformed.returncode, 1)

    def test_changed_manifest_is_rejected_by_cng(self) -> None:
        _, payload, output = self.new_case()
        self.assertEqual(self.run_helper(payload, output).returncode, 0)
        manifest = output / "manifest.json"
        changed = bytearray(manifest.read_bytes())
        # Keep the JSON and all manifest fields valid: only the source commit changes.
        offset = changed.index(COMMIT.encode("ascii"))
        changed[offset] = ord("1")  # Original COMMIT starts with '0'.
        manifest.write_bytes(changed)
        self.assertEqual(json.loads(manifest.read_bytes().decode("utf-8"))["commit"], "1" + COMMIT[1:])
        self.assertEqual(
            self.run_verifier(self.public_keys[0], manifest, output / "manifest.sig").returncode,
            1,
        )

    def test_changed_der_signature_is_rejected_by_cng(self) -> None:
        _, payload, output = self.new_case()
        self.assertEqual(self.run_helper(payload, output).returncode, 0)
        signature = output / "manifest.sig"
        changed = bytearray(signature.read_bytes())
        self.assertGreater(len(changed), 10)
        changed[10] ^= 0x01
        signature.write_bytes(changed)
        self.assertEqual(
            self.run_verifier(self.public_keys[0], output / "manifest.json", signature).returncode,
            1,
        )

    def test_other_valid_p256_key_rejects_genuine_signature(self) -> None:
        _, payload, output = self.new_case()
        self.assertEqual(self.run_helper(payload, output).returncode, 0)
        self.assertEqual(
            self.run_verifier(self.public_keys[1], output / "manifest.json", output / "manifest.sig").returncode,
            1,
        )

    def test_missing_empty_and_malformed_public_keys_publish_nothing(self) -> None:
        _, payload, output = self.new_case()
        missing_key = output.parent / "missing-public-key.hex"
        result = self.run_helper(payload, output, public_key=missing_key)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())

        for index, key_text in enumerate((b"", b"04not-a-p256-public-key\n")):
            with self.subTest(index=index):
                invalid_key = output.parent / f"invalid-public-key-{index}.hex"
                invalid_key.write_bytes(key_text)
                result = self.run_helper(payload, output, public_key=invalid_key)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(output.exists())

    def test_mismatched_private_and_public_keys_publish_nothing(self) -> None:
        _, payload, output = self.new_case()
        result = self.run_helper(
            payload,
            output,
            private_key=self.private_keys[0],
            public_key=self.public_keys[1],
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())
        self.assertIn(b"does not match", result.stderr)

    def test_missing_private_key_path_publishes_nothing(self) -> None:
        _, payload, output = self.new_case()
        result = self.run_helper(payload, output, private_key=payload.parent / "missing.pem")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())

    def test_private_key_cli_argument_is_required(self) -> None:
        _, payload, output = self.new_case()
        result = self.run_helper(payload, output, include_private_key=False)
        self.assertEqual(result.returncode, 2)
        self.assertFalse(output.exists())

    def test_output_inside_payload_is_rejected(self) -> None:
        _, payload, _ = self.new_case()
        output = payload / "published"
        result = self.run_helper(payload, output)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())

    def test_private_key_inside_payload_is_rejected(self) -> None:
        _, payload, output = self.new_case()
        embedded_key = payload / "private.pem"
        shutil.copyfile(self.private_keys[0], embedded_key)
        result = self.run_helper(payload, output, private_key=embedded_key)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())

    def test_existing_output_sentinel_is_preserved(self) -> None:
        _, payload, output = self.new_case()
        output.mkdir()
        sentinel = output / "sentinel.txt"
        sentinel.write_bytes(b"preserve existing output")
        result = self.run_helper(payload, output)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sentinel.read_bytes(), b"preserve existing output")
        self.assertEqual({path.name for path in output.iterdir()}, {"sentinel.txt"})

    def test_payload_mutation_after_manifest_generation_is_rejected(self) -> None:
        _, payload, output = self.new_case()
        original_run = subprocess.run
        mutated = False

        def run_with_mutation(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[bytes]:
            nonlocal mutated
            result = original_run(command, **kwargs)
            if (
                not mutated
                and result.returncode == 0
                and Path(command[0]).resolve() == self.manifest_tool
            ):
                file = payload / "layerd.exe"
                changed = bytearray(file.read_bytes())
                changed[0] ^= 0x01
                file.write_bytes(changed)
                mutated = True
            return result

        with mock.patch.object(release_preparation.subprocess, "run", side_effect=run_with_mutation):
            with self.assertRaises(release_preparation.ReleasePreparationError):
                release_preparation.prepare_release(
                    root=payload,
                    manifest_tool=self.manifest_tool,
                    private_key=self.private_keys[0],
                    public_key=self.public_keys[0],
                    output=output,
                    channel=CHANNEL,
                    sequence=SEQUENCE,
                    commit=COMMIT,
                    openssl=self.openssl,
                )
        self.assertTrue(mutated)
        self.assertFalse(output.exists())

    def test_payload_symlink_is_rejected_when_supported(self) -> None:
        _, payload, output = self.new_case()
        target = payload / "licenses" / "LICENSE.txt"
        link = payload / "docs" / "license-link.txt"
        try:
            os.symlink(target, link)
        except (NotImplementedError, OSError):
            self.skipTest("symlink creation is unavailable for this user")
        result = self.run_helper(payload, output)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())


def main() -> int:
    parse_arguments()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(UpdateReleaseTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
