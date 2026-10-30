import argparse
import copy
import hashlib
import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).parents[1] / "desktop_evidence_layerc.py"
SPEC = importlib.util.spec_from_file_location("desktop_evidence_layerc", MODULE_PATH)
assert SPEC and SPEC.loader
subject = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = subject
SPEC.loader.exec_module(subject)
validator = subject.subject
FIXTURES = Path(__file__).with_name("fixtures") / "desktop_evidence"
CONFIG_PATH = Path(__file__).parents[2] / ".github/desktop-evidence.json"
REPOSITORY = "EnderKeera/LayerC"
COMMIT = "a" * 40


class LayerCAdapterTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.windows = self.root / "windows"
        self.linux = self.root / "linux"
        shutil.copytree(FIXTURES / "windows-results", self.windows)
        shutil.copytree(FIXTURES / "linux-evidence/20260520T123456Z-gnome-native", self.linux)
        self.identity = self.windows / "layerc-build-identity.json"
        self.config = validator.load_config(CONFIG_PATH)

    def tearDown(self):
        self.directory.cleanup()

    def windows_args(self, **changes):
        values = dict(results=self.windows, identity=self.identity, commit=COMMIT,
                      repository=REPOSITORY, os="Windows Test", gpu="Example GPU", machine="physical",
                      visual_only=False, recorded_at="2026-05-20T12:34:56Z")
        values.update(changes)
        return argparse.Namespace(**values)

    def linux_args(self, **changes):
        values = dict(evidence=self.linux, desktop="gnome", package_mode="native", commit=COMMIT,
                      repository=REPOSITORY, os="Example Linux", machine="physical")
        values.update(changes)
        return argparse.Namespace(**values)

    def update_json(self, path, **changes):
        value = json.loads(path.read_text(encoding="utf-8"))
        value.update(changes)
        path.write_text(json.dumps(value), encoding="utf-8")
        return value

    def assert_valid_round_trip(self, report):
        self.assertEqual([], validator.validate_report(report, self.config, REPOSITORY))
        comment = validator.render_comment(report, self.config)
        parsed = validator.parse_report(validator.extract_report(comment))
        self.assertEqual(report, parsed)
        self.assertEqual([], validator.validate_report(parsed, self.config, REPOSITORY))
        for secret in ("SECRET-HOST", "SECRET-USER", "C:/", str(self.root)):
            self.assertNotIn(secret, comment)

    def run_cli(self, command, *arguments):
        return subprocess.run([sys.executable, str(MODULE_PATH), command, "--config", str(CONFIG_PATH),
                               "--repository", REPOSITORY, "--commit", COMMIT, "--os", "Example OS",
                               "--machine", "physical", "--out", str(self.root / "report.json"),
                               *map(str, arguments)], capture_output=True, text=True, encoding="utf-8")

    def test_windows_maps_real_benchmark_shape_and_hashes_exact_identity_bytes(self):
        report = subject.windows_report(self.windows_args())
        self.assertEqual(list(subject.WINDOWS_IDS), [result["id"] for result in report["results"]])
        self.assertEqual(["pass", "pass", "fail", "pass"], [result["status"] for result in report["results"]])
        self.assertEqual(["passed", "passed", "thresholds not met", "passed"],
                         [result["detail"] for result in report["results"]])
        self.assertEqual({"source": "ci-artifact", "ciRunId": 123456, "builtCommit": "c" * 40,
                          "identitySha256": hashlib.sha256(self.identity.read_bytes()).hexdigest()}, report["build"])
        self.assertNotEqual(report["commit"], report["build"]["builtCommit"])
        self.assert_valid_round_trip(report)
        self.identity.write_bytes(self.identity.read_bytes() + b"\r\n")
        changed = subject.windows_report(self.windows_args())
        self.assertNotEqual(report["build"]["identitySha256"], changed["build"]["identitySha256"])
        self.assert_valid_round_trip(changed)

    def test_windows_missing_results_and_visual_only(self):
        for test_id in ("v11-visual-low", "v22-latency"):
            (self.windows / (test_id + ".json")).unlink()
        report = subject.windows_report(self.windows_args())
        self.assertEqual("fail", report["results"][0]["status"])
        self.assertEqual("no result: the benchmark did not complete", report["results"][0]["detail"])
        self.assertEqual("fail", report["results"][3]["status"])
        visual = subject.windows_report(self.windows_args(visual_only=True))
        self.assertEqual("fail", visual["results"][0]["status"])
        self.assertEqual({"id": "v22-latency", "status": "skip", "detail": "not run (VisualOnly)"}, visual["results"][3])
        self.assert_valid_round_trip(visual)
        self.assertEqual("failed", validator.evaluate(visual, self.config["harnesses"]["layerc-windows-hardware"])[0])

    def test_windows_identity_mismatches_are_failed_results(self):
        path = self.windows / "v11-visual-low.json"
        original = json.loads(path.read_text(encoding="utf-8"))
        for field, value in (("source", "local-build"), ("runId", "999"), ("commitSha", COMMIT)):
            result = copy.deepcopy(original)
            result["build"][field] = value
            path.write_text(json.dumps(result), encoding="utf-8")
            with self.subTest(field=field):
                report = subject.windows_report(self.windows_args())
                self.assertEqual({"id": "v11-visual-low", "status": "fail",
                                  "detail": "result build identity does not match the CI artifact"}, report["results"][0])
                self.assert_valid_round_trip(report)
        result["build"] = None
        path.write_text(json.dumps(result), encoding="utf-8")
        self.assertEqual("fail", subject.windows_report(self.windows_args())["results"][0]["status"])
        result = copy.deepcopy(original)
        result["build"]["runId"] = 123456
        path.write_text(json.dumps(result), encoding="utf-8")
        self.assertEqual("pass", subject.windows_report(self.windows_args())["results"][0]["status"])

    def test_windows_unreadable_and_non_boolean_passed(self):
        path = self.windows / "v11-visual-low.json"
        original = json.loads(path.read_text(encoding="utf-8"))
        for content in ("{", "[]", '{"x": NaN}', '{"x": 1, "x": 2}',
                        '{"deep":' + '[' * 10000 + '0' + ']' * 10000 + '}'):
            path.write_text(content, encoding="utf-8")
            result = subject.windows_report(self.windows_args())["results"][0]
            self.assertEqual("fail", result["status"])
            self.assertEqual("unreadable result", result["detail"])
        for value in (None, "true", 1, 0, [], {}):
            original["passed"] = value
            path.write_text(json.dumps(original), encoding="utf-8")
            result = subject.windows_report(self.windows_args())["results"][0]
            self.assertEqual("fail", result["status"])
            self.assertEqual("result has no boolean passed", result["detail"])
        del original["passed"]
        path.write_text(json.dumps(original), encoding="utf-8")
        self.assertEqual("result has no boolean passed", subject.windows_report(self.windows_args())["results"][0]["detail"])

    def test_windows_invalid_identity_exits_cleanly(self):
        original = self.identity.read_bytes()
        for field, value in (("source", "local-build"), ("commitSha", "not-a-sha"),
                             ("runId", "bad"), ("runId", "-1"), ("runId", "１２３"),
                             ("runId", 123), ("runId", True), ("runId", "9" * 5000)):
            self.identity.write_bytes(original)
            self.update_json(self.identity, **{field: value})
            result = self.run_cli("windows-report", "--results", self.windows, "--identity", self.identity)
            with self.subTest(field=field, value=str(value)[:40]):
                # Older Python versions do not impose an integer-string length limit.
                expected = 1 if field != "runId" or len(str(value)) < 5000 or sys.version_info >= (3, 11) else 2
                self.assertEqual(expected, result.returncode, result.stderr)
                self.assertNotIn("Traceback", result.stderr)
                self.assertFalse((self.root / "report.json").exists())
        self.identity.write_text("{", encoding="utf-8")
        result = self.run_cli("windows-report", "--results", self.windows, "--identity", self.identity)
        self.assertEqual(1, result.returncode, result.stderr)

    def test_sanitizer_drops_unsafe_text_collapses_spaces_and_truncates(self):
        report = subject.windows_report(self.windows_args(os="  Windows` |<Test>\n\u202e\u00a0\ud800  OS  ",
                                                         gpu="\t<GPU> | `\u202e", recorded_at=None))
        self.assertEqual("Windows Test OS", report["environment"]["os"])
        self.assertEqual("GPU", report["environment"]["gpu"])
        self.assertTrue(validator.matches(validator.RECORDED_AT, report["recordedAt"]))
        self.assert_valid_round_trip(report)
        report = subject.windows_report(self.windows_args(os="\n`|<>\u202e", gpu="\n<|>"))
        self.assertEqual("unknown", report["environment"]["os"])
        self.assertNotIn("gpu", report["environment"])
        for maximum in (80, 200):
            text = subject.sanitize_text(" x" * 300, maximum)
            self.assertLessEqual(len(text), maximum)
            self.assertTrue(validator.valid_text(text, maximum))

    def test_windows_cli_validates_produced_report(self):
        result = self.run_cli("windows-report", "--results", self.windows, "--identity", self.identity)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assert_valid_round_trip(json.loads((self.root / "report.json").read_text(encoding="utf-8")))
        for arguments in (("--machine", "vm"), ("--commit", "bad"), ("--recorded-at", "2026-02-30T12:00:00Z")):
            (self.root / "report.json").unlink(missing_ok=True)
            result = self.run_cli("windows-report", "--results", self.windows, "--identity", self.identity, *arguments)
            self.assertEqual(2, result.returncode, result.stderr)
            self.assertFalse((self.root / "report.json").exists())

    def test_linux_maps_summary_stages_time_and_clean_tree(self):
        report = subject.linux_report(self.linux_args())
        self.assertEqual("layerc-linux-x11-native", report["harness"]["id"])
        self.assertEqual("gnome-xorg", report["environment"]["session"])
        self.assertEqual("2026-05-20T12:34:56Z", report["recordedAt"])
        self.assertEqual({"source": "local-build", "builtCommit": COMMIT, "dirty": False}, report["build"])
        self.assertEqual(["pass", "pass", "pass", "skip", "pass"], [result["status"] for result in report["results"]])
        self.assertEqual("operator recorded pass", report["results"][0]["detail"])
        self.assert_valid_round_trip(report)

    def test_linux_manual_fail_and_pending(self):
        for value, status, detail, evaluation in (("pass", "pass", "operator recorded pass", "complete"),
                                                 ("fail", "fail", "operator recorded fail", "failed"),
                                                 ("pending", "unavailable", "operator result pending", "incomplete")):
            self.update_json(self.linux / "summary.json", manualResult=value)
            report = subject.linux_report(self.linux_args())
            self.assertEqual({"id": "manual-checklist", "status": status, "detail": detail}, report["results"][0])
            self.assertEqual(evaluation, validator.evaluate(report, self.config["harnesses"][report["harness"]["id"]])[0])
            self.assert_valid_round_trip(report)

    def test_linux_dirty_detection_and_unbound_evidence(self):
        (self.linux / "automated-summary.json").unlink()
        self.update_json(self.linux / "summary.json", automatedEvidence="not-bound")
        for content, dirty in (("\n  \n", False), (" M src/example.cpp\n", True), ("?? scratch.txt\n", True)):
            (self.linux / "git-status.txt").write_text(content, encoding="utf-8")
            report = subject.linux_report(self.linux_args())
            self.assertEqual(dirty, report["build"]["dirty"])
            for result in report["results"][1:]:
                self.assertEqual("unavailable", result["status"])
                self.assertEqual("automated evidence not bound", result["detail"])
            self.assert_valid_round_trip(report)

    def test_linux_automated_statuses_and_sanitized_details(self):
        path = self.linux / "automated-summary.json"
        for value, status in (("pass", "pass"), ("fail", "fail"), ("skip", "skip"), ("skipped", "skip"),
                              ("pending", "unavailable"), ("<bad>`|\n\u202e", "unavailable"), (None, "unavailable")):
            self.update_json(path, automatedEvidence={"verify-headless": {"status": value}})
            report = subject.linux_report(self.linux_args())
            self.assertEqual(status, report["results"][1]["status"])
            self.assertTrue(report["results"][1]["detail"].startswith("automated stage status "))
            self.assertEqual("unavailable", report["results"][2]["status"])
            self.assert_valid_round_trip(report)

    def test_linux_kde_flatpak_includes_flatpak_stage(self):
        self.update_json(self.linux / "summary.json", desktop="kde", packageMode="flatpak")
        report = subject.linux_report(self.linux_args(desktop="kde", package_mode="flatpak"))
        self.assertEqual("layerc-linux-x11-flatpak", report["harness"]["id"])
        self.assertEqual("kde-x11", report["environment"]["session"])
        self.assertEqual("flatpak", report["results"][-1]["id"])
        self.assertEqual("pass", report["results"][-1]["status"])
        self.assert_valid_round_trip(report)

    def test_linux_cli_mismatches_exit_one_and_normal_output_validates(self):
        arguments = ("--evidence", self.linux, "--desktop", "gnome", "--package-mode", "native")
        result = self.run_cli("linux-report", *arguments)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assert_valid_round_trip(json.loads((self.root / "report.json").read_text(encoding="utf-8")))
        for options, reason in ((("--desktop", "kde"), "desktop"),
                                (("--package-mode", "flatpak"), "package mode"),
                                (("--commit", "b" * 40), "git-head.txt")):
            result = self.run_cli("linux-report", *arguments, *options)
            self.assertEqual(1, result.returncode, result.stderr)
            self.assertIn(reason, result.stderr)
            self.assertNotIn("Traceback", result.stderr)
        self.update_json(self.linux / "summary.json", manualResult="other")
        self.assertEqual(1, self.run_cli("linux-report", *arguments).returncode)


if __name__ == "__main__":
    unittest.main()
