"""Failure-injection tests for tools/publish-rolling.sh (layer44 B08-03).

The script runs against a fake `gh` that keeps the release, its assets, and
the rolling tag in a temporary directory. The real repository is never
contacted. Every mutation can be made to fail before it applies, and the
tests require that each interrupted state is either the previous consistent
publication or one that `--verify` rejects, and that a rerun converges.
"""

import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "publish-rolling.sh"
CANONICAL = "layerc-windows-x64.zip"
OLD_SHA = "1" * 40
NEW_SHA = "2" * 40
API = "repos/{owner}/{repo}/"

FAKE_GH = textwrap.dedent(
    r'''
    #!/usr/bin/env python3
    import hashlib, json, os, shutil, sys
    from pathlib import Path

    root = Path(os.environ["FAKE_GH_STATE"])
    state_file = root / "state.json"
    state = json.loads(state_file.read_text())
    api = "repos/{owner}/{repo}/"
    args = sys.argv[1:]
    assets_query = ".assets[] | [.id, .name] | @tsv"


    def reject(message="unsupported gh invocation"):
        print(f"fake gh: {message}: {args!r}", file=sys.stderr)
        sys.exit(2)


    def not_found():
        print("gh: Not Found (HTTP 404)", file=sys.stderr)
        sys.exit(1)


    def mutate(operation):
        counts = state.setdefault("counts", {})
        counts[operation] = counts.get(operation, 0) + 1
        failure = os.environ.get("FAKE_GH_FAIL_AT", "")
        if failure:
            name, _, nth = failure.partition(":")
            if name == operation and counts[operation] == int(nth or 1):
                save()
                print(f"fake gh: injected {operation} failure", file=sys.stderr)
                sys.exit(1)
        state.setdefault("log", []).append(operation)


    def save():
        state_file.write_text(json.dumps(state, indent=1, sort_keys=True))


    def store(source, name):
        data = Path(source).read_bytes()
        if os.environ.get("FAKE_GH_CORRUPT_UPLOAD"):
            data = data + b"corrupted in transit"
        asset_id = state["next_id"]
        state["next_id"] += 1
        (root / "assets" / str(asset_id)).write_bytes(data)
        state["release"]["assets"].append({"id": asset_id, "name": name})


    def asset(asset_id):
        for item in state["release"]["assets"]:
            if item["id"] == asset_id:
                return item
        not_found()


    def option(name):
        if name not in args:
            reject(f"missing {name}")
        return args[args.index(name) + 1]


    release = state.get("release")
    if args[:1] == ["api"]:
        if args[1:3] in (["-X", "DELETE"], ["-X", "PATCH"], ["-X", "POST"]):
            method, path = args[2], args[3].removeprefix(api)
            if method == "DELETE" and path.startswith("releases/assets/") and len(args) == 4:
                item = asset(int(path.rsplit("/", 1)[1]))
                mutate("delete_asset")
                state["release"]["assets"].remove(item)
                (root / "assets" / str(item["id"])).unlink()
            elif method == "PATCH" and path.startswith("releases/assets/") and args[4:5] == ["-f"]:
                item = asset(int(path.rsplit("/", 1)[1]))
                key, _, value = args[5].partition("=")
                if key != "name" or len(args) != 6:
                    reject()
                if any(other["name"] == value for other in release["assets"]):
                    reject("asset name already exists")
                mutate("rename_asset")
                item["name"] = value
            elif method == "PATCH" and path == "git/refs/tags/rolling":
                if args[4:] != ["-f", args[5], "-F", "force=true"] or not args[5].startswith("sha="):
                    reject()
                if state.get("tag") is None:
                    not_found()
                mutate("move_tag")
                state["tag"] = args[5].removeprefix("sha=")
            elif method == "POST" and path == "git/refs":
                if args[4:] != ["-f", "ref=refs/tags/rolling", "-f", args[7]]:
                    reject()
                mutate("create_tag")
                state["tag"] = args[7].removeprefix("sha=")
            else:
                reject()
            save()
            sys.exit(0)
        if len(args) != 4 or args[2] != "--jq":
            reject()
        path, query = args[1].removeprefix(api), args[3]
        if path == "releases/tags/rolling":
            if release is None:
                not_found()
            if query == ".id":
                print(1)
            elif query == ".body":
                print(release["body"])
            elif query == assets_query:
                for item in release["assets"]:
                    print(f'{item["id"]}\t{item["name"]}')
            else:
                reject("unsupported query")
        elif path == "git/ref/tags/rolling" and query == ".object.sha":
            if state.get("tag") is None:
                not_found()
            print(state["tag"])
        elif path.startswith("compare/") and query == ".status":
            print(state.get("compare", "ahead"))
        else:
            reject()
        sys.exit(0)

    if args[:3] == ["release", "create", "rolling"]:
        if release is not None:
            reject("release exists")
        state["release"] = {"assets": [], "body": "", "title": "", "prerelease": False}
        mutate("create")
        store(args[3], Path(args[3]).name)
        state["tag"] = option("--target")
        state["release"].update(title=option("--title"), body=option("--notes"),
                                prerelease="--prerelease" in args)
        save()
    elif args[:3] == ["release", "upload", "rolling"] and len(args) == 4:
        if release is None:
            not_found()
        if any(item["name"] == Path(args[3]).name for item in release["assets"]):
            reject("upload without --clobber over an existing asset")
        mutate("upload")
        store(args[3], Path(args[3]).name)
        save()
    elif args[:3] == ["release", "download", "rolling"] and len(args) == 7:
        name, directory = option("--pattern"), option("--dir")
        for item in release["assets"]:
            if item["name"] == name:
                shutil.copyfile(root / "assets" / str(item["id"]), Path(directory) / name)
                sys.exit(0)
        not_found()
    elif args[:3] == ["release", "edit", "rolling"]:
        if release is None or "--target" in args:
            reject("edit must not imply retargeting")
        mutate("edit")
        release.update(title=option("--title"), body=option("--notes"),
                       prerelease="--prerelease" in args)
        save()
    else:
        reject()
    '''
).lstrip()


def notes(sha, digest):
    return (
        f"Rolling Windows x64 build of main@{sha}; every ci.yml gate is green.\n\n"
        f"{CANONICAL} SHA-256: {digest}"
    )


@unittest.skipUnless(
    os.name != "nt" and shutil.which("bash") and shutil.which("sha256sum"),
    "needs a POSIX bash with sha256sum",
)
class PublishRollingTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="layerc-rolling-"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        fake = self.bin / "gh"
        fake.write_text(FAKE_GH)
        fake.chmod(0o755)
        self.state_root = self.root / "state"
        (self.state_root / "assets").mkdir(parents=True)
        self.save_state({"release": None, "tag": None, "next_id": 1})
        self.input = self.root / "input"
        self.input.mkdir()
        self.package = b"new package bytes"
        (self.input / "layerc-2.6.0-windows-x64.zip").write_bytes(self.package)

    def save_state(self, state):
        (self.state_root / "state.json").write_text(json.dumps(state))

    def state(self):
        return json.loads((self.state_root / "state.json").read_text())

    def snapshot(self):
        files = {p.name: p.read_bytes() for p in (self.state_root / "assets").iterdir()}
        return (self.state_root / "state.json").read_bytes(), files

    def add_asset(self, name, data):
        state = self.state()
        asset_id = state["next_id"]
        state["next_id"] += 1
        (self.state_root / "assets" / str(asset_id)).write_bytes(data)
        state["release"]["assets"].append({"id": asset_id, "name": name})
        self.save_state(state)

    def publish_old(self, extra=()):
        old = b"old package bytes"
        self.save_state({
            "release": {"assets": [], "title": "LayerC rolling build",
                        "prerelease": True,
                        "body": notes(OLD_SHA, hashlib.sha256(old).hexdigest())},
            "tag": OLD_SHA, "next_id": 1})
        self.add_asset(CANONICAL, old)
        for name, data in extra:
            self.add_asset(name, data)

    def run_script(self, *args, sha=NEW_SHA, **env):
        environment = dict(os.environ, PATH=f"{self.bin}{os.pathsep}{os.environ['PATH']}",
                           FAKE_GH_STATE=str(self.state_root), **env)
        environment.pop("GITHUB_SHA", None)
        if sha is not None:
            environment["GITHUB_SHA"] = sha
        return subprocess.run(["bash", str(SCRIPT), *args], env=environment,
                              capture_output=True, text=True, timeout=60)

    def assert_verified(self, sha, data):
        result = self.run_script("--verify")
        self.assertEqual(result.returncode, 0, result.stderr)
        state = self.state()
        self.assertEqual(state["tag"], sha)
        self.assertEqual([a["name"] for a in state["release"]["assets"]], [CANONICAL])
        self.assertIn(hashlib.sha256(data).hexdigest(), state["release"]["body"])
        self.assertTrue(state["release"]["prerelease"])

    def test_first_publication_creates_a_consistent_release(self):
        result = self.run_script(str(self.input))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_verified(NEW_SHA, self.package)

    def test_promotion_replaces_strays_moves_tag_and_commits_notes(self):
        self.publish_old(extra=[("layerc-2.5.0-windows-x64.zip", b"stray"),
                                (f"layerc-windows-x64.{OLD_SHA}.staged", b"stale")])
        result = self.run_script(str(self.input))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_verified(NEW_SHA, self.package)
        self.assertEqual(self.state()["log"][-2:], ["move_tag", "edit"])

    def test_missing_tag_is_created_not_patched(self):
        self.publish_old()
        state = self.state()
        state["tag"] = None
        self.save_state(state)
        result = self.run_script(str(self.input))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_verified(NEW_SHA, self.package)

    def test_an_older_commit_never_replaces_a_newer_publication(self):
        self.publish_old()
        state = self.state()
        state["compare"] = "behind"
        self.save_state(state)
        before = self.snapshot()
        result = self.run_script(str(self.input))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("newer", result.stdout)
        self.assertEqual(self.snapshot(), before)

    def test_corrupted_staging_never_touches_the_published_zip(self):
        self.publish_old()
        result = self.run_script(str(self.input), FAKE_GH_CORRUPT_UPLOAD="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("delete_asset", self.state().get("log", []))
        self.assertEqual(self.run_script("--verify").returncode, 0)
        self.assertEqual(self.state()["tag"], OLD_SHA)
        result = self.run_script(str(self.input))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_verified(NEW_SHA, self.package)

    def test_invalid_inputs_fail_without_mutation(self):
        self.publish_old()
        before = self.snapshot()
        cases = [dict(sha=None), dict(sha="abc123"), dict(sha="AB" * 20)]
        for case in cases:
            with self.subTest(sha=case["sha"]):
                self.assertNotEqual(self.run_script(str(self.input), **case).returncode, 0)
        empty = self.root / "empty"
        empty.mkdir()
        self.assertNotEqual(self.run_script(str(empty)).returncode, 0)
        (self.input / "second.zip").write_bytes(b"second")
        self.assertNotEqual(self.run_script(str(self.input)).returncode, 0)
        self.assertEqual(self.snapshot(), before)

    def test_every_interrupted_mutation_is_detectable_and_recoverable(self):
        self.publish_old(extra=[(f"layerc-windows-x64.{OLD_SHA}.staged", b"stale")])
        initial = self.snapshot()
        self.assertEqual(self.run_script(str(self.input)).returncode, 0)
        operations = self.state()["log"]
        self.assertEqual(operations, ["delete_asset", "upload", "delete_asset",
                                      "rename_asset", "move_tag", "edit"])
        seen = {}
        for operation in operations:
            seen[operation] = seen.get(operation, 0) + 1
            point = f"{operation}:{seen[operation]}"
            with self.subTest(fail_at=point):
                state_json, files = initial
                (self.state_root / "state.json").write_bytes(state_json)
                shutil.rmtree(self.state_root / "assets")
                (self.state_root / "assets").mkdir()
                for name, data in files.items():
                    (self.state_root / "assets" / name).write_bytes(data)

                failed = self.run_script(str(self.input), FAKE_GH_FAIL_AT=point)
                self.assertNotEqual(failed.returncode, 0)
                state = self.state()
                state.pop("counts", None)
                self.save_state(state)
                if self.run_script("--verify").returncode == 0:
                    # Only the untouched previous publication may verify.
                    self.assertEqual(state["tag"], OLD_SHA)
                    self.assertIn(OLD_SHA, state["release"]["body"])
                    self.assertNotIn(hashlib.sha256(self.package).hexdigest(),
                                     state["release"]["body"])
                recovered = self.run_script(str(self.input))
                self.assertEqual(recovered.returncode, 0, recovered.stderr)
                self.assert_verified(NEW_SHA, self.package)
                self.assertFalse(any(a["name"].endswith(".staged")
                                     for a in self.state()["release"]["assets"]))


if __name__ == "__main__":
    unittest.main()
